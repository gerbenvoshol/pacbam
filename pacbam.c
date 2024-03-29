#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>
#include "samtools/sam.h"
#include "samtools/faidx.h"
#include "hashmap.h"

/////////////////////////////
///CIGAR related macros
/////////////////////////////

#define BAM_CMATCH      0
#define BAM_CINS        1
#define BAM_CDEL        2
#define BAM_CREF_SKIP   3
#define BAM_CSOFT_CLIP  4
#define BAM_CHARD_CLIP  5
#define BAM_CPAD        6
#define BAM_CEQUAL      7
#define BAM_CDIFF       8
#define BAM_CBACK       9
#define BAM_CIGAR_SHIFT 4
#define BAM_CIGAR_MASK  0xf

#define bam_cigar_op(c) ((c)&BAM_CIGAR_MASK)
#define bam_cigar_oplen(c) ((c)>>BAM_CIGAR_SHIFT)

#ifdef _WIN32
#include <windows.h>
#define sleep(x) Sleep(1000 * x)
#endif

int MAX_READ_BUFF = 1000000;
int MAX_DUP = 1;
int MAX_CHR = 10000;
int MAX_CSOFT_CLIP = 101;
char **BED_CHR;
char **VCF_CHR;
char **ORD_CHR;
int max_isize = 0;

#define CHUNK_SIZE 32768

///////////////////////////////////////////////////////////
// Dedup hasmap data structures
///////////////////////////////////////////////////////////

#define KEY_MAX_LENGTH (128)

typedef struct dedup_struct_s {
	char key_string[KEY_MAX_LENGTH];
	int32_t chr1;
	int32_t chr2;
	int32_t pos_r1;
	int32_t pos_r2;
	int bp;
	int paired;
	int isize;
} dedup_struct_t;


typedef struct dup_struct_s {
	char key_string[KEY_MAX_LENGTH];
	char name[KEY_MAX_LENGTH];
	int bp;
} dup_struct_t;


typedef struct fetch_reads_s {
	bam_plbuf_t *buf;
	map_t *hmap;
} fetch_reads_t;

/////////////////////////////////////////////////////////////////////////////////////
// Normal CDF
/////////////////////////////////////////////////////////////////////////////////////

double erf(double x)
{
	double y = 1.0 / (1.0 + 0.3275911 * x);
	return 1 - (((((
	                   + 1.061405429  * y
	                   - 1.453152027) * y
	               + 1.421413741) * y
	              - 0.284496736) * y
	             + 0.254829592) * y)
	       * exp(-x * x);
}

double pdf(double x, double mu, double sigma)
{

	const double pi = 3.14159265;
	return exp(-1 * (x - mu) * (x - mu) / (2 * sigma * sigma)) / (sigma * sqrt(2 * pi));
}

double cdf(double x, double mu, double sigma)
{
	return 0.5 * (1 + erf((x - mu) / (sigma * sqrt(2.))));
}

///////////////////////////////////////////////////////////
// Input parameters
///////////////////////////////////////////////////////////

struct input_args {
	char *bed;
	char *vcf;
	char *bam;
	char *fasta;
	char *outdir;
	char *duptablename;
	int dedup;
	int mode;
	int cores;
	int mbq;
	int mrq;
	int mdc;
	int genotype;
	//int dupmode;
	int strand_bias;
	int dedup_window;
	float region_perc;
};


struct input_args *getInputArgs(char *argv[], int argc)
{
	int i;
	struct input_args *arguments = (struct input_args *)malloc(sizeof(struct input_args));
	arguments->cores = 1;
	arguments->mbq = 20;
	arguments->mrq = 1;
	arguments->mdc = 0;
	arguments->mode = 6;
	//arguments->dupmode = 0;
	arguments->strand_bias = 0;
	arguments->genotype = 0;
	arguments->vcf = NULL;
	arguments->bed = NULL;
	arguments->bam = NULL;
	arguments->fasta = NULL;
	arguments->duptablename = NULL;
	arguments->dedup = 0;
	arguments->dedup_window = 1000;
	arguments->outdir = (char *)malloc(3);
	sprintf(arguments->outdir, "./");
	arguments->region_perc = 0.5;

	char *tmp = NULL;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "mode=", 5) == 0) {
			tmp = (char*)malloc(strlen(argv[i]) - 3);
			strcpy(tmp, argv[i] + 5);
			arguments->mode = atoi(tmp);
			free(tmp);
			// mode 6 requires strand_bias to calculate StrandA, StrandC, StrandG and StrandT
			// The fraction of forward reads supporting the call
			if (arguments->mode == 6) {
				arguments->strand_bias = 1;
			}
		} else if (strncmp(argv[i], "bed=", 4) == 0) {
			arguments->bed = (char*)malloc(sizeof(char) * strlen(argv[i]) + 1);
			strcpy(arguments->bed, argv[i] + 4);
			subSlash(arguments->bed);
		} else if (strncmp(argv[i], "vcf=", 4) == 0) {
			arguments->vcf = (char*)malloc(sizeof(char) * strlen(argv[i]) + 1);
			strcpy(arguments->vcf, argv[i] + 4);
			subSlash(arguments->vcf);
		} else if (strncmp(argv[i], "bam=", 4) == 0) {
			arguments->bam = (char*)malloc(sizeof(char) * strlen(argv[i]) + 1);
			strcpy(arguments->bam, argv[i] + 4);
			subSlash(arguments->bam);
		} else if (strncmp(argv[i], "fasta=", 6) == 0) {
			arguments->fasta = (char*)malloc(sizeof(char) * strlen(argv[i]) + 1);
			strcpy(arguments->fasta, argv[i] + 6);
			subSlash(arguments->fasta);
		} else if (strncmp(argv[i], "duptab=", 7) == 0) {
			arguments->duptablename = (char*)malloc(sizeof(char) * strlen(argv[i]) + 1);
			strcpy(arguments->duptablename, argv[i] + 7);
			subSlash(arguments->duptablename);
		} else if (strncmp(argv[i], "threads=", 8) == 0) {
			tmp = (char*)malloc(strlen(argv[i]) - 7);
			strcpy(tmp, argv[i] + 8);
			arguments->cores = atoi(tmp);
			free(tmp);
		} else if (strncmp(argv[i], "mbq=", 4) == 0) {
			tmp = (char*)malloc(strlen(argv[i]) - 3);
			strcpy(tmp, argv[i] + 4);
			arguments->mbq = atoi(tmp);
			free(tmp);
		} else if (strncmp(argv[i], "mrq=", 4) == 0) {
			tmp = (char*)malloc(strlen(argv[i]) - 3);
			strcpy(tmp, argv[i] + 4);
			arguments->mrq = atoi(tmp);
			free(tmp);
		} else if (strncmp(argv[i], "mdc=", 4) == 0) {
			tmp = (char*)malloc(strlen(argv[i]) - 3);
			strcpy(tmp, argv[i] + 4);
			arguments->mdc = atoi(tmp);
			free(tmp);
		} else if (strncmp(argv[i], "out=", 4) == 0) {
			arguments->outdir = (char*)malloc(strlen(argv[i]) - 3);
			strcpy(arguments->outdir, argv[i] + 4);
			subSlash(arguments->outdir);
		} else if (strncmp(argv[i], "regionperc=", 11) == 0) {
			tmp = (char*)malloc(strlen(argv[i]) - 10);
			strcpy(tmp, argv[i] + 11);
			arguments->region_perc = atof(tmp);
			free(tmp);
		} else if (strncmp(argv[i], "strandbias", 10) == 0) {
			arguments->strand_bias = 1;
		} else if (strncmp(argv[i], "dedup", 6) == 0) {
			arguments->dedup = 1;
		} else if (strncmp(argv[i], "dedupwin=", 9) == 0) {
			tmp = (char*)malloc(strlen(argv[i]) - 8);
			strcpy(tmp, argv[i] + 9);
			arguments->dedup_window = atoi(tmp);
			free(tmp);
		} else if (strncmp(argv[i], "genotype", 8) == 0) {
			arguments->genotype = 1;
		} else if (strncmp(argv[i], "genotypeBT", 16) == 0) {
			arguments->genotype = 2;
		} else {
			fprintf(stderr, "ERROR: input parameters not valid.\n");
			exit(1);
		}
	}
	return arguments;
}

// Check existance of a file
int checkFileExistance(char *file_name)
{
	FILE *file;
	if (file_name == NULL) {
		return (1);
	}
	if (fopen(file_name, "r") == NULL) {
		return (2);
	}
	return (0);
}

int checkInputArgs(struct input_args *arguments)
{
	int control = 0;
	int vcf_control = 0;
	const char ch = '.';
	char *ret;

	if (arguments->cores <= 0) {
		fprintf(stderr, "ERROR: the number of threads is not valid.\n");
		control = 1;
	}
	if (checkFileExistance(arguments->bed) > 0) {
		fprintf(stderr, "ERROR: File BED does not exist or is not specified.\n");
		control = 1;
	}
	ret = strrchr(arguments->bed, ch);
	if (strncmp(ret, ".bed", 4) != 0) {
		fprintf(stderr, "ERROR: A file BED should be specified.\n");
		control = 1;
	}
	if (checkFileExistance(arguments->bam) > 0) {
		fprintf(stderr, "ERROR: File BAM does not exist or is not specified.\n");
		control = 1;
	}
	if (vcf_control = checkFileExistance(arguments->vcf) == 2) {
		fprintf(stderr, "ERROR: File VCF does not exist.\n");
	}
	ret = strrchr(arguments->vcf, ch);
	if (strncmp(ret, ".vcf", 4) != 0) {
		fprintf(stderr, "ERROR: A file VCF should be specified.\n");
		vcf_control == 1;
	}
	if (checkFileExistance(arguments->fasta) > 0) {
		fprintf(stderr, "ERROR: File FASTA does not exist or is not specified.\n");
		control = 1;
	}
	if (checkFileExistance(arguments->duptablename) == 2) {
		fprintf(stderr, "ERROR: File Duplicates table does not exist.\n");
		control = 1;
	}
	if (arguments->mbq < 0) {
		fprintf(stderr, "ERROR: minimum base quality should be positive.\n");
		control = 1;
	}
	if (arguments->mrq < 0) {
		fprintf(stderr, "ERROR: minimum read quality should be positive.\n");
		control = 1;
	}
	if (arguments->mdc < 0) {
		fprintf(stderr, "ERROR: minimum depth of coverage should be positive.\n");
		control = 1;
	}
	if (arguments->mode < 0 || arguments->mode > 6) {
		fprintf(stderr, "ERROR: mode should be in 0,1,2,3,4,5,6.\n");
		control = 1;
	}
	if (arguments->region_perc < 0 || arguments->region_perc > 1) {
		fprintf(stderr, "ERROR: Region fraction should be in the range [0,1].\n");
		control = 1;
	}
	if ((arguments->mode == 0 | arguments->mode == 1 | arguments->mode == 2 | arguments->mode == 5) && vcf_control == 1) {
		fprintf(stderr, "ERROR: Selected mode requires the specification of a VCF file.\n");
		control = 1;
	}
	if (control == 1) {
		return 1;
	}
	return 0;
}

void printArguments(struct input_args *arguments)
{
	if (arguments->duptablename != NULL) {
		fprintf(stderr, " BAM=%s\n BED=%s\n VCF=%s\n FASTA=%s\n DUPTAB=%s\n MAXDUP=%d\n MODE=%d\n MBQ=%d\n MRQ=%d\n MDC=%d\n THREADS=%d\n OUT=%s\n REGIONPERC=%f\n",
		        arguments->bam, arguments->bed, arguments->vcf, arguments->fasta, arguments->duptablename, MAX_DUP, arguments->mode,
		        arguments->mbq, arguments->mrq, arguments->mdc, arguments->cores, arguments->outdir, arguments->region_perc);
	} else {
		fprintf(stderr, " BAM=%s\n BED=%s\n VCF=%s\n FASTA=%s\n MODE=%d\n MBQ=%d\n MRQ=%d\n MDC=%d\n THREADS=%d\n OUT=%s\n REGIONPERC=%f\n",
		        arguments->bam, arguments->bed, arguments->vcf, arguments->fasta, arguments->mode,
		        arguments->mbq, arguments->mrq, arguments->mdc, arguments->cores, arguments->outdir, arguments->region_perc);
	}
}

void printHelp()
{
	fprintf(stderr, "\nUsage: \n ./pacbam bam=string bed=string vcf=string fasta=string [mode=int] [threads=int] [mbq=int] [mrq=int] [mdc=int] [out=string] [dedup] [dedupwin=int] [regionperc=float] [strandbias]\n\n");
	fprintf(stderr, "bam=string \n NGS data file in BAM format\n");
	fprintf(stderr, "bed=string \n List of target captured regions in BED format\n");
	fprintf(stderr, "vcf=string \n List of SNP positions in VCF format (no compressed files are admitted)\n");
	fprintf(stderr, "fasta=string \n Reference genome FASTA format file \n");
	fprintf(stderr, "mode=string \n Execution mode [0=RC+SNPs+SNVs|1=RC+SNPs+SNVs+PILEUP(not including SNPs)|2=SNPs|3=RC|4=PILEUP|6=BAMCOUNT]\n (default 4)\n");
	fprintf(stderr, "dedup \n On-the-fly duplicates filtering\n");
	fprintf(stderr, "dedupwin=int \n Flanking region around captured regions to consider in duplicates filtering [default 1000]\n");
	fprintf(stderr, "threads=int \n Number of threads used (if available) for the pileup computation\n (default 1)\n");
	fprintf(stderr, "regionperc=float \n Fraction of the captured region to consider for maximum peak signal characterization\n (default 0.5)\n");
	fprintf(stderr, "mbq=int \n Min base quality\n (default 20)\n");
	fprintf(stderr, "mrq=int \n Min read quality\n (default 1)\n");
	fprintf(stderr, "mdc=int \n Min depth of coverage that a position should have to be considered in the output\n (default 0)\n");
	fprintf(stderr, "strandbias \n Print strand bias count information\n");
	fprintf(stderr, "genotype \n Print genotype calls for input SNPs using a strategy based on an allelic fraction cutoff threshold at 20%\n");
	fprintf(stderr, "genotypeBT \n Print genotype calls for input SNPs using a strategy based on a binomial test with significance at 1%)\n");
	fprintf(stderr, "out=string \n Path of output directory (default is the current directory)\n\n");
}


///////////////////////////////////////////////////////////
// Duplicates list
///////////////////////////////////////////////////////////

struct Dnode {
	int pos;
	int pos_pair;
	int chr_pair;
	int number;
	int strand;
	struct Dlist *next;
};

typedef struct Dnode Dlist;

Dlist *addDlist(Dlist *list, int *list_counts, int npos, int npos_pair, int nchr_pair, int nstrand)
{
	Dlist *tmp = list;
	// Assumes reads in the pileup area ordered by position
	//if(list!=NULL && npos>(list->pos+MAX_CSOFT_CLIP))
	//{
	//	countAndFreeDlist(tmp,list_counts);
	//	list = NULL;
	//}

	tmp = list;
	while (list != NULL) {
		if (list->pos == npos && list->pos_pair == npos_pair && list->chr_pair == nchr_pair && list->strand == nstrand) {
			list->number++;
			return (tmp);
		}
		list = list->next;
	}
	Dlist *link = (struct Dnode*)malloc(sizeof(struct Dnode));
	link->pos = npos;
	link->pos_pair = npos_pair;
	link->chr_pair = nchr_pair;
	link->number = 1;
	link->strand = nstrand;
	link->next = tmp;
	return (link);
}

void countAndFreeDlist(Dlist *list, int *list_counts)
{
	int idx;
	Dlist *tmp;
	while (list != NULL) {
		idx = list->number - 1;
		if (idx > (MAX_DUP - 1)) {
			idx = MAX_DUP - 1;
		}
		list_counts[idx]++;
		tmp = list;
		list = list->next;
		free(tmp);
	}
}

void printDlist(Dlist *list)
{
	if (list == NULL) {
		fprintf(stderr, "NA");
	}
	while (list != NULL) {
		fprintf(stderr, "%d-%d:%d", list->pos, list->pos_pair, list->number);
		list = list->next;
		if (list != NULL) {
			fprintf(stderr, "|");
		}
	}
}


///////////////////////////////////////////////////////////
// Utilities
///////////////////////////////////////////////////////////

int lastSlash(char *s)
{
	int i;
	int pos = -1;
#ifdef _WIN32
	for (i = 0; i < strlen(s); i++)
		if (s[i] == '\\') {
			pos = i;
		}
#else
	for (i = 0; i < strlen(s); i++)
		if (s[i] == '/') {
			pos = i;
		}
#endif
	return pos;
}

void subSlash(char *s)
{
	int i;
	int pos = -1;
#ifdef _WIN32
	for (i = 0; i < strlen(s); i++)
		if (s[i] == '/') {
			s[i] = '\\';
		}
#else
	for (i = 0; i < strlen(s); i++)
		if (s[i] == '\\') {
			s[i] = "/";
		}
#endif
}

void printMessage(char *message)
{
	time_t current_time;
	char* c_time_string;
	current_time = time(NULL);
	c_time_string = ctime(&current_time);
	c_time_string[strlen(c_time_string) - 1] = '\0';
	fprintf(stderr, "[%s] %s\n", c_time_string, message);
}

int isInteger(char *s)
{
	int i;
	for (i = 0; i < strlen(s); i++)
		if (!(s[i] >= '0' && s[i] <= '9')) {
			return 1;
		}
	return 0;
}


///////////////////////////////////////////////////////////
// Regions and single base position info
///////////////////////////////////////////////////////////

// duplicates filter lookup table
struct lookup_dup {
	int length;
	int *covs_up;
	int *covs_down;
	int *thresholds;
};


int getThrDupLookup(struct lookup_dup *duptable, int cov)
{
	int i;
	if (cov < duptable->covs_down[0]) {
		return (duptable->thresholds[0]);
	}
	if (cov >= duptable->covs_up[(duptable->length) - 1]) {
		return (duptable->thresholds[(duptable->length) - 1]);
	}
	for (i = 0; i < duptable->length; i++) {
		if (cov >= duptable->covs_down[i] && cov < duptable->covs_up[i]) {
			return (duptable->thresholds[i]);
		}
	}
	return (10000);
}

struct pos_pileup {
	uint32_t pos; // Max 4,294,967,295
	int A;        // Count of fwd mapped read. Max 2,147,483,647
	int C;
	int G;
	int T;
	int Asb;      // Count of rc mapped read
	int Csb;
	int Gsb;
	int Tsb;
	int del;      // Count deletions
};

// Data for a single region
struct region_data {
	uint32_t beg;
	uint32_t end;
	struct pos_pileup *positions;
	samfile_t *in;
	struct lookup_dup *duptable;
	struct input_args *arguments;
};

// Contains info of a captured region
struct target_t {
	char *chr;
	uint32_t from;
	uint32_t to;
	struct region_data *rdata;
	char *sequence;
	uint32_t from_sel;
	uint32_t to_sel;
	float gc;
	float read_count;
	float read_count_global;
};

// Collects info of all captured regions
struct target_info {
	int length;
	struct target_t **info;
};

// Contains info of the snps
struct snps_t {
	char *chr;
	uint32_t pos;
	char *rsid;
	char *ref;
	char *alt;
};

// Collects info of all snps
struct snps_info {
	int length;
	struct snps_t **info;
};

char getBase(int val)
{
	if (val == 1) {
		return ('A');
	} else if (val == 2) {
		return ('C');
	} else if (val == 4) {
		return ('G');
	} else if (val == 8) {
		return ('T');
	}
}

void incBase(struct pos_pileup *elem, int val)
{
	if (val == 1) {
		elem->A++;
	} else if (val == 2) {
		elem->C++;
	} else if (val == 4) {
		elem->G++;
	} else if (val == 8) {
		elem->T++;
	}
}

void incBaseStrand(struct pos_pileup *elem, int val, int strand)
{
	if (val == 1 && strand) {
		elem->Asb++;
	} else if (val == 2 && strand) {
		elem->Csb++;
	} else if (val == 4 && strand) {
		elem->Gsb++;
	} else if (val == 8 && strand) {
		elem->Tsb++;
	}
}


// Increments the number of duplicated reads per base
void incBaseDup(Dlist *dup[], int *dup_counts[], int val, int pos, int pos_pair, int chr_pair, int strand)
{
	if (val == 1) {
		dup[0] = addDlist(dup[0], dup_counts[0], pos, pos_pair, chr_pair, strand);
	} else if (val == 2) {
		dup[1] = addDlist(dup[1], dup_counts[1], pos, pos_pair, chr_pair, strand);
	} else if (val == 4) {
		dup[2] = addDlist(dup[2], dup_counts[2], pos, pos_pair, chr_pair, strand);
	} else if (val == 8) {
		dup[3] = addDlist(dup[3], dup_counts[3], pos, pos_pair, chr_pair, strand);
	}
}

// Compute coverage of bases by duplicates lists
void computeCovBaseDup(int *dup_counts[], struct pos_pileup *elem, int thr_cov_A, int thr_cov_C, int thr_cov_G, int thr_cov_T)
{
	int i;
	elem->A = 0;
	for (i = 0; i < MAX_DUP; i++) {
		if (i <= (thr_cov_A - 1)) {
			elem->A += (i + 1) * dup_counts[0][i];
		} else {
			elem->A += thr_cov_A * dup_counts[0][i];
		}
	}
	elem->C = 0;
	for (i = 0; i < MAX_DUP; i++) {
		if (i <= (thr_cov_C - 1)) {
			elem->C += (i + 1) * dup_counts[1][i];
		} else {
			elem->C += thr_cov_C * dup_counts[1][i];
		}
	}
	elem->G = 0;
	for (i = 0; i < MAX_DUP; i++) {
		if (i <= (thr_cov_G - 1)) {
			elem->G += (i + 1) * dup_counts[2][i];
		} else {
			elem->G += thr_cov_G * dup_counts[2][i];
		}
	}
	elem->T = 0;
	for (i = 0; i < MAX_DUP; i++) {
		if (i <= (thr_cov_T - 1)) {
			elem->T += (i + 1) * dup_counts[3][i];
		} else {
			elem->T += thr_cov_T * dup_counts[3][i];
		}
	}
}

void mergeBEDVCFCHRLists(char **vcf, char **bed, char **merge)
{
	int i = 0, j = 0, k = 0, l, n;
	while (vcf[i] != NULL) {
		l = j;
		while (bed[l] != NULL) {
			if (strcmp(vcf[i], bed[l]) == 0) {
				break;
			}
			l++;
		}
		if (bed[l] == NULL) {
			merge[k] = (char *)malloc(sizeof(char) * strlen(vcf[i]) + 1);
			strcpy(merge[k], vcf[i]);
			subSlash(merge[k]);
			k++;
			i++;
		} else {
			for (n = j; n <= l; n++) {
				merge[k] = (char *)malloc(sizeof(char) * strlen(bed[n]) + 1);
				strcpy(merge[k], bed[n]);
				subSlash(merge[k]);
				k++;
			}
			j = l + 1;
			i++;
		}
	}
	while (bed[j] != NULL) {
		merge[k] = (char *)malloc(sizeof(char) * strlen(bed[j]) + 1);
		strcpy(merge[k], bed[j]);
		subSlash(merge[k]);
		k++;
		j++;
	}
}

void printCHR(char **elems)
{
	time_t current_time;
	char* c_time_string;
	current_time = time(NULL);
	c_time_string = ctime(&current_time);
	c_time_string[strlen(c_time_string) - 1] = '\0';
	fprintf(stderr, "[%s] Loaded chromosomes: ", c_time_string);
	int i = 0;
	while (elems[i] != NULL) {
		fprintf(stderr, "%s", elems[i]);
		if (elems[i + 1] != NULL) {
			fprintf(stderr, ",");
		}
		i++;
	}
	fprintf(stderr, "\n");
}

int getChrPos(char *chr)
{
	int i = 0;
	while (ORD_CHR[i] != NULL) {
		if (strcmp(ORD_CHR[i], chr) == 0) {
			return (i);
		}
		i++;
	}
}

int getBaseCount(struct region_data *elem, char *s, int index)
{
	if (strncmp(s, "A", 1) == 0) {
		return (elem->positions[index].A);
	} else if (strncmp(s, "C", 1) == 0) {
		return (elem->positions[index].C);
	} else if (strncmp(s, "G", 1) == 0) {
		return (elem->positions[index].G);
	} else if (strncmp(s, "T", 1) == 0) {
		return (elem->positions[index].T);
	}
	return (0);
}

int getAlternativeSum(struct region_data *elem, char *s, int index)
{
	if (strncmp(s, "A", 1) == 0) {
		return (elem->positions[index].C + elem->positions[index].G + elem->positions[index].T);
	} else if (strncmp(s, "C", 1) == 0) {
		return (elem->positions[index].A + elem->positions[index].G + elem->positions[index].T);
	} else if (strncmp(s, "G", 1) == 0) {
		return (elem->positions[index].A + elem->positions[index].C + elem->positions[index].T);
	} else if (strncmp(s, "T", 1) == 0) {
		return (elem->positions[index].A + elem->positions[index].C + elem->positions[index].G);
	}
	return (0);
}

int getSum(struct region_data *elem, int index)
{
	return (elem->positions[index].A + elem->positions[index].C + elem->positions[index].G + elem->positions[index].T);
}


int CheckAlt(struct target_t *elem, int index)
{
	if (elem->sequence == NULL) {
		return (0);
	}
	if (elem->sequence[index] == 'A') {
		return (elem->rdata->positions[index].C + elem->rdata->positions[index].G + elem->rdata->positions[index].T);
	} else if (elem->sequence[index] == 'C') {
		return (elem->rdata->positions[index].A + elem->rdata->positions[index].G + elem->rdata->positions[index].T);
	} else if (elem->sequence[index] == 'G') {
		return (elem->rdata->positions[index].A + elem->rdata->positions[index].C + elem->rdata->positions[index].T);
	} else if (elem->sequence[index] == 'T') {
		return (elem->rdata->positions[index].A + elem->rdata->positions[index].C + elem->rdata->positions[index].G);
	}
	return (0);
}

// Returns read count of alternative allele
int FindAlternative(struct region_data *elem, char *s, int index, char *alt)
{
	int max = 0;

	// Reference is specified
	if (strncmp(s, "A", 1) == 0) {
		sprintf(alt, "C");
		max = elem->positions[index].C;
		if (max < elem->positions[index].G) {
			max = elem->positions[index].G;
			sprintf(alt, "G");
		}
		if (max < elem->positions[index].T) {
			max = elem->positions[index].T;
			sprintf(alt, "T");
		}
		if ((max == elem->positions[index].C) + (max == elem->positions[index].G) + (max == elem->positions[index].T) > 1) {
			sprintf(alt, "N");
			return (0);
		}
		return (max);
	}

	if (strncmp(s, "C", 1) == 0) {
		sprintf(alt, "A");
		max = elem->positions[index].A;
		if (max < elem->positions[index].G) {
			max = elem->positions[index].G;
			sprintf(alt, "G");
		}
		if (max < elem->positions[index].T) {
			max = elem->positions[index].T;
			sprintf(alt, "T");
		}
		if ((max == elem->positions[index].A) + (max == elem->positions[index].G) + (max == elem->positions[index].T) > 1) {
			sprintf(alt, "N");
			return (0);
		}
		return (max);
	}

	if (strncmp(s, "G", 1) == 0) {
		sprintf(alt, "C");
		max = elem->positions[index].C;
		if (max < elem->positions[index].A) {
			max = elem->positions[index].A;
			sprintf(alt, "A");
		}
		if (max < elem->positions[index].T) {
			max = elem->positions[index].T;
			sprintf(alt, "T");
		}
		if ((max == elem->positions[index].C) + (max == elem->positions[index].A) + (max == elem->positions[index].T) > 1) {
			sprintf(alt, "N");
			return (0);
		}
		return (max);
	}

	if (strncmp(s, "T", 1) == 0) {
		sprintf(alt, "C");
		max = elem->positions[index].C;
		if (max < elem->positions[index].G) {
			max = elem->positions[index].G;
			sprintf(alt, "G");
		}
		if (max < elem->positions[index].A) {
			max = elem->positions[index].A;
			sprintf(alt, "A");
		}
		if ((max == elem->positions[index].C) + (max == elem->positions[index].G) + (max == elem->positions[index].A) > 1) {
			sprintf(alt, "N");
			return (0);
		}
		return (max);
	}

	return (0);
}


///////////////////////////////////////////////////////////
// Samtools pileup function definition
///////////////////////////////////////////////////////////

int32_t interpret_cigar(int32_t pos, uint32_t *cigar, int n_cigar)
{
	int i, op, ol;
	for (i = 0; i < n_cigar; i++) {
		op = bam_cigar_op(cigar[i]);
		ol = bam_cigar_oplen(cigar[i]);
		if (op == BAM_CMATCH || op == BAM_CDIFF || op == BAM_CEQUAL || op == BAM_CREF_SKIP || op == BAM_CDEL) {
			pos += ol;
		}
	}
	return (pos);
}

void getKey(dedup_struct_t* value, char *key)
{
	int32_t p1, p2, c1, c2;
	if (value->paired == 1) {

		if (value->chr1 == value->chr2) {
			c1 = c2 = value->chr1;
			if (value->pos_r1 <= value->pos_r2) {
				p1 = value->pos_r1;
				p2 = value->pos_r2;
			} else {
				p1 = value->pos_r2;
				p2 = value->pos_r1;
			}
		} else {
			if (value->chr1 < value->chr2) {
				p1 = value->pos_r1;
				c1 = value->chr1;
				p2 = value->pos_r2;
				c2 = value->chr2;
			} else {
				p1 = value->pos_r2;
				c1 = value->chr2;
				p2 = value->pos_r1;
				c2 = value->chr1;
			}
		}

		snprintf(key, KEY_MAX_LENGTH, "%d-%d:%d-%d", c1, c2, p1, p2);
	} else {
		if (value->pos_r1 < 0) {
			p1 = value->pos_r2;
			c1 = value->chr2;
		} else {
			p1 = value->pos_r1;
			c1 = value->chr1;
		}

		snprintf(key, KEY_MAX_LENGTH, "%d:%d", c1, p1);
	}
}


// callback for bam_fetch()
static int fetch_func_dup(const bam1_t *b, void *data)
{
	map_t *hmap = (map_t *)data;
	uint32_t *cigar;
	char *name;
	int32_t pos;
	int res, strand, op, ol, error;
	dedup_struct_t* value;

	name = bam1_qname(b);
	cigar = bam1_cigar(b);
	strand = bam1_strand(b);

	res = hashmap_get(hmap, name, (void**)(&value));
	if (res == MAP_MISSING) {
		value = malloc(sizeof(dedup_struct_t));
		snprintf(value->key_string, KEY_MAX_LENGTH, "%s", name);
		value->pos_r1 = value->pos_r2 = -1;
		value->chr1 = value->chr2 = -1;
		value->paired = 0;
		value->isize = abs(b->core.isize);
		if (b->core.flag & BAM_FPAIRED) {
			value->paired = 1;
		}
		if (b->core.flag & BAM_FMUNMAP) {
			value->paired = 0;
		}
	}

	if (strand == 0) {
		pos = b->core.pos;
		op = bam_cigar_op(cigar[0]);
		ol = bam_cigar_oplen(cigar[0]);
		if (op == BAM_CSOFT_CLIP) {
			pos = pos - ol;
		}
		if (value->pos_r1 < 0) {
			value->pos_r1 = pos;
			value->chr1 = b->core.tid;
		} else {
			value->pos_r2 = pos;
			value->chr2 = b->core.tid;
		}
		value->bp = interpret_cigar(pos, cigar, b->core.n_cigar) - pos;
		//fprintf(stderr,"%d %d %d\n",interpret_cigar(pos,cigar,b->core.n_cigar),pos,value->bp);
	} else {
		pos = interpret_cigar(b->core.pos, cigar, b->core.n_cigar);
		op = bam_cigar_op(cigar[b->core.n_cigar - 1]);
		ol = bam_cigar_oplen(cigar[b->core.n_cigar - 1]);
		if (op == BAM_CSOFT_CLIP) {
			pos = pos + ol;
		}
		if (value->pos_r2 < 0) {
			value->pos_r2 = pos;
			value->chr2 = b->core.tid;
		} else {
			value->pos_r1 = pos;
			value->chr1 = b->core.tid;
		}
		value->bp += (pos - b->core.pos);
		//fprintf(stderr,"%d %d %d\n",interpret_cigar(pos,cigar,b->core.n_cigar),pos,value->bp);
	}

	if (res == MAP_MISSING) {
		error = hashmap_put(hmap, value->key_string, value);
		assert(error == MAP_OK);
	}

	return 0;
}

// callback for bam_fetch()
static int fetch_func_dedup(const bam1_t *b, void *data)
{
	fetch_reads_t *buf_data = (fetch_reads_t *)data;
	dedup_struct_t* value;
	char *name = bam1_qname(b);
	int res;
	res = hashmap_get(buf_data->hmap, name, (void**)(&value));

	if (res == MAP_OK) {
		//fprintf(stderr,"USED: %s\n",value->key_string);
		bam_plbuf_push(b, buf_data->buf);
	}
	return 0;
}

// callback for bam_fetch()
static int fetch_func(const bam1_t *b, void *data)
{
	bam_plbuf_t *buf = (bam_plbuf_t * *)data;
	bam_plbuf_push(b, buf);
	return 0;
}


// callback for bam_plbuf_init()
static int pileup_func(uint32_t tid, uint32_t pos, int n, const bam_pileup1_t *pl, void *data)
{
	int i;
	struct region_data *tmp = (struct region_data*)data;
	unsigned char *qual;
	int read_init;
	uint32_t *cigar, lcigar;
	uint8_t *md;
	int thr_cov_A, thr_cov_C, thr_cov_G, thr_cov_T, op, ol, pos_c;
	Dlist *dup[4];
	int *dup_counts[4];
	/*if (tmp->arguments->duptablename != NULL)
	{
		dup[0] = dup[1] = dup[2] = dup[3] = NULL;
		dup_counts[0] = (int *)malloc(sizeof(int)*MAX_DUP);
		dup_counts[1] = (int *)malloc(sizeof(int)*MAX_DUP);
		dup_counts[2] = (int *)malloc(sizeof(int)*MAX_DUP);
		dup_counts[3] = (int *)malloc(sizeof(int)*MAX_DUP);
		for(i=0;i<MAX_DUP;i++)
		{
			dup_counts[0][i] = dup_counts[1][i] = dup_counts[2][i] = dup_counts[3][i] = 0;
		}
	}*/

	if ((int)pos >= tmp->beg && (int)pos < tmp->end) {
		for (i = 0; i < n; i++) {
			qual = bam1_qual(pl[i].b);
			if (!(
			            qual[pl[i].qpos] < tmp->arguments->mbq ||
			            pl[i].b->core.qual < tmp->arguments->mrq ||
			            pl[i].is_del != 0 ||
			            //pl[i].indel != 0)
			            pl[i].is_refskip != 0 ||
			            (pl[i].b->core.flag & BAM_DEF_MASK))) {
				incBase(&(tmp->positions[pos - tmp->beg]), bam1_seqi(bam1_seq(pl[i].b), pl[i].qpos));
				if (tmp->arguments->strand_bias == 1) {
					incBaseStrand(&(tmp->positions[pos - tmp->beg]), bam1_seqi(bam1_seq(pl[i].b), pl[i].qpos), bam1_strand(pl[i].b));
				}
				/*if (tmp->arguments->duptablename != NULL)
				{
					cigar = bam1_cigar(pl[i].b);
					op = bam_cigar_op(cigar[0]);
					ol = bam_cigar_oplen(cigar[0]);
					pos_c = pl[i].b->core.pos;
					if(op==BAM_CSOFT_CLIP)
						pos_c = pos_c-ol;
					incBaseDup(&dup,&dup_counts,bam1_seqi(bam1_seq(pl[i].b),pl[i].qpos),pos_c+1,(pl[i].b->core.mpos)+1,pl[i].b->core.mtid,bam1_strand(pl[i].b));
				}*/
			} else if (pl[i].is_del) {
				tmp->positions[pos - tmp->beg].del++;
			}
		}


		/*if (tmp->arguments->duptablename != NULL)
		{
			// find duplicates threshdold for all bases coverage
			thr_cov_A = getThrDupLookup(tmp->duptable,tmp->positions[pos-tmp->beg].A);
			thr_cov_C = getThrDupLookup(tmp->duptable,tmp->positions[pos-tmp->beg].C);
			thr_cov_G = getThrDupLookup(tmp->duptable,tmp->positions[pos-tmp->beg].G);
			thr_cov_T = getThrDupLookup(tmp->duptable,tmp->positions[pos-tmp->beg].T);

			// recompute base coverage after lifting
			countAndFreeDlist(dup[0],dup_counts[0]);
			countAndFreeDlist(dup[1],dup_counts[1]);
			countAndFreeDlist(dup[2],dup_counts[2]);
			countAndFreeDlist(dup[3],dup_counts[3]);

			computeCovBaseDup(&dup_counts,&(tmp->positions[pos-tmp->beg]),thr_cov_A,thr_cov_C,thr_cov_G,thr_cov_T);
			free(dup_counts[0]);free(dup_counts[1]);free(dup_counts[2]);free(dup_counts[3]);
		}*/

	}

	return 0;
}





///////////////////////////////////////////////////////////
// Regions RC processing
///////////////////////////////////////////////////////////

float computeGC(char *sequence, int init, int end)
{
	int i;
	int count = 0;
	for (i = init; i <= end; i++) {
		if (sequence[i] == 'G' || sequence[i] == 'C') {
			count++;
		}
	}
	return (((float)count) / ((float)(end - init + 1)));
}

void computeRC(float perc, struct target_t *region)
{
	int positions = (int)(floor((float)(region->to - region->from) * perc));
	int init = 0;
	int end = positions - 1;
	float max = 0;
	int i, sum, sumt;


	if (positions == 0) {
		sum = region->rdata->positions[init].A +
		      region->rdata->positions[init].C +
		      region->rdata->positions[init].G +
		      region->rdata->positions[init].T;
		region->read_count = region->read_count_global = sum;
		region->gc = computeGC(region->sequence, 0, 0);
		region->from_sel = region->from;
		region->to_sel = region->to;
		return;
	}
	region->from_sel = init + region->from;
	region->to_sel = end + region->from;
	sumt = 0;

	while (end < (region->to - region->from) && end >= 0) {
		sum = 0;
		sumt += region->rdata->positions[init].A +
		        region->rdata->positions[init].C +
		        region->rdata->positions[init].G +
		        region->rdata->positions[init].T;
		for (i = init; i <= end; i++)
			sum += region->rdata->positions[i].A +
			       region->rdata->positions[i].C +
			       region->rdata->positions[i].G +
			       region->rdata->positions[i].T;
		if ((float)sum > max) {
			region->from_sel = init + region->from;
			region->to_sel = end + region->from;
			max = (float)sum;

		}
		init++;
		end++;
	}

	for (i = init; i <= (region->to - region->from); i++)
		sumt += region->rdata->positions[i].A +
		        region->rdata->positions[i].C +
		        region->rdata->positions[i].G +
		        region->rdata->positions[i].T;

	region->read_count = (float)max / (float)positions;
	region->read_count_global = (float)sumt / ((float)(region->to - region->from));
}

void computeGCRegion(float perc, struct target_t *region)
{
	int positions = (int)(floor((float)(region->to - region->from)));
	if (positions == 0) {
		positions = 1;
	}
	if (region->sequence != NULL) {
		region->gc = computeGC(region->sequence, 0, positions - 1);
	}
}


///////////////////////////////////////////////////////////
// Load Target BED file
///////////////////////////////////////////////////////////

struct lookup_dup* loadDUPLookupTable(char *file_name)
{
	FILE *file = fopen(file_name, "r");
	char *seq, *tmp_seq, s[200], stmp[200];
	int i, len;

	if (file == NULL) {
		fprintf(stderr, "\nFile %s not present!!!\n", file_name);
		exit(1);
	}

	// get number of lines
	int number_of_lines = 0;

	char line [MAX_READ_BUFF];
	while (fgets(line, sizeof(line), file) != NULL) {
		int i = 0;
		while (isspace(line[i])) {
			i++;
		}
		if (line[i] != '#') {
			number_of_lines++;
		}
	}

	rewind(file);

	struct lookup_dup *table = (struct lookup_dup *)malloc(sizeof(struct lookup_dup));
	table->covs_down = malloc(sizeof(int) * number_of_lines);
	table->covs_up = malloc(sizeof(int) * number_of_lines);
	table->thresholds = malloc(sizeof(int) * number_of_lines);
	table->length = number_of_lines;

	int index = 0;
	int control = 0;
	int columns = 0;
	char sep[] = "\t";

	int line_numb = 1;

	while (fgets(line, sizeof(line), file) != NULL) {
		if (control == 0) {
			// count the number of columns
			for (i = 0; i < strlen(line); i++) {
				if (strncmp(&line[i], sep, 1) == 0) {
					columns++;
				}
			}
			columns++;
			control = 1;
		}

		// tokenize a line
		char *str_tokens[columns];
		char *pch;
		pch = strtok(line, "\t");
		i = 0;
		while (pch != NULL) {
			str_tokens[i++] = pch;
			pch = strtok(NULL, "\t");
		}

		if (i < 3) {
			fprintf(stderr, "ERROR: at line %d the number of columns is not correct (at least 3 columns required).\n", line_numb);
			exit(1);
		}

		table->covs_down[index] = atoi(str_tokens[0]);
		table->covs_up[index] = atoi(str_tokens[1]);
		table->thresholds[index] = atoi(str_tokens[2]);
		if (table->thresholds[index] > MAX_DUP) {
			MAX_DUP = table->thresholds[index];
		}

		if (table->covs_down[index] < 0 || table->covs_up[index] < 0 || table->thresholds[index] < 0) {
			fprintf(stderr, "ERROR: at line %d values are not correct).\n", line_numb);
			exit(1);
		}
		if (table->covs_down[index] >= table->covs_up[index]) {
			fprintf(stderr, "ERROR: at line %d values do not define a coverage interval.\n", line_numb);
			exit(1);
		}

		index++;
		line_numb++;
	}

	return (table);
}

struct target_info* loadTargetBed(char *file_name)
{
	FILE *file = fopen(file_name, "r");
	char *seq, *tmp_seq, s[200], stmp[200], prev_chr[20];
	int i, len, idx_chr;
	uint32_t prev_to = 0;

	if (file == NULL) {
		fprintf(stderr, "\nFile %s not present.\n", file_name);
		exit(1);
	}

	// get number of lines
	int number_of_lines = 0;

	char line [MAX_READ_BUFF];
	while (fgets(line, sizeof(line), file) != NULL) {
		int i = 0;
		while (isspace(line[i])) {
			i++;
		}
		if (line[i] != '#') {
			number_of_lines++;
		}
	}

	rewind(file);

	if (number_of_lines == 0) {
		fprintf(stderr, "ERROR: BED file is empty.\n");
		exit(1);
	}

	// create the overall structure
	struct target_info *target = (struct target_info *)malloc(sizeof(struct target_info));
	target->info = malloc(sizeof(struct target_t *)*number_of_lines);
	target->length = number_of_lines;

	int index = 0;
	int control = 0;
	int columns = 0;
	char sep[] = "\t";
	char sep_intervals[] = ",";

	int line_numb = 1;
	idx_chr = 0;
	sprintf(prev_chr, "init");

	while (fgets(line, sizeof(line), file) != NULL) {
		if (control == 0) {
			// count the number of columns
			for (i = 0; i < strlen(line); i++) {
				if (strncmp(&line[i], sep, 1) == 0) {
					columns++;
				}
			}
			columns++;
			control = 1;
		}

		// tokenize a line
		char *str_tokens[columns];
		char *pch;
		pch = strtok(line, "\t");
		i = 0;
		while (pch != NULL) {
			str_tokens[i++] = pch;
			pch = strtok(NULL, "\t");
		}

		if (i < 3) {
			fprintf(stderr, "ERROR: at line %d the number of columns is not correct (at least 3 columns required).\n", line_numb);
			exit(1);
		}

		struct target_t *current_elem = (struct target_t *)malloc(sizeof(struct target_t));
		int size;
		current_elem->chr = (char*)malloc(strlen(str_tokens[0]) + 1);
		strcpy(current_elem->chr, str_tokens[0]);
		current_elem->from = strtol(str_tokens[1], &pch, 10);
		current_elem->to = strtol(str_tokens[2], &pch, 10);

		// check correct conversion
		sprintf(stmp, "%u", current_elem->from);
		if (strncmp(stmp, str_tokens[1], strlen(stmp)) != 0) {
			fprintf(stderr, "ERROR: genomic coordinates at line %d are not valid.\n", line_numb);
			exit(1);
		}
		current_elem->from++;

		sprintf(stmp, "%u", current_elem->to);
		if (strncmp(stmp, str_tokens[2], strlen(stmp)) != 0) {
			fprintf(stderr, "ERROR: genomic coordinates at line %d are not valid.\n", line_numb);
			exit(1);
		}

		// check ordering
		if (strcmp(current_elem->chr, prev_chr) != 0) {
			i = 0;
			while (BED_CHR[i] != NULL) {
				if (strcmp(BED_CHR[i], current_elem->chr) == 0) {
					fprintf(stderr, "ERROR: chromosomes are not ordered (line %d).\n", line_numb);
					exit(1);
				}
				i++;
			}
			sprintf(prev_chr, "%s", current_elem->chr);
			BED_CHR[idx_chr] = (char *)malloc(sizeof(char) * strlen(current_elem->chr) + 1);
			strcpy(BED_CHR[idx_chr], current_elem->chr);
			subSlash(BED_CHR[idx_chr]);
			idx_chr++;
			prev_to = 0;
		} else {
			if (current_elem->from <= prev_to) {
				fprintf(stderr, "ERROR: genomic regions are not positionally ordered or there are overlapping regions (line %d).\n", line_numb);
				exit(1);
			}

			prev_to = current_elem->to;
		}

		if (current_elem->from > current_elem->to) {
			fprintf(stderr, "ERROR: genomic region at line %d has inverted coordinates.\n", line_numb);
			exit(1);
		}
		if (current_elem->to - current_elem->from < 0) {
			fprintf(stderr, "ERROR: genomic region at line %d has length <1.\n", line_numb);
			exit(1);
		}


		if (str_tokens[3][strlen(str_tokens[3]) - 1] == '\n') {
			size = strlen(str_tokens[3]) - 1;
		} else {
			size = strlen(str_tokens[3]);
		}

		current_elem->sequence = NULL;
		current_elem->rdata = NULL;
		current_elem->sequence = NULL;
		current_elem->from_sel = 0;
		current_elem->to_sel = 0;
		current_elem->gc = 0;
		current_elem->read_count = 0;
		current_elem->read_count_global = 0;

		target->info[index] = current_elem;

		index++;
		line_numb++;
	}

	return (target);
}


///////////////////////////////////////////////////////////
// Load SNPs file
///////////////////////////////////////////////////////////

struct snps_info* loadSNPs(char *file_name)
{
	FILE *file = fopen(file_name, "r");
	char *seq, *tmp_seq, s[200], stmp[200], prev_chr[20];
	int i, len, prev_pos, idx_chr;
	char strA[] = "A";
	char strC[] = "C";
	char strG[] = "G";
	char strT[] = "T";

	if (file == NULL) {
		fprintf(stderr, "\nFile %s not present.\n", file_name);
		exit(1);
	}

	// get number of lines
	int number_of_lines = 0;

	char line [MAX_READ_BUFF];
	while (fgets(line, sizeof(line), file) != NULL) {
		int i = 0;
		while (isspace(line[i])) {
			i++;
		}
		if (line[i] != '#') {
			number_of_lines++;
		}
	}

	rewind(file);

	if (number_of_lines == 0) {
		fprintf(stderr, "ERROR: VCF file is empty.\n");
		exit(1);
	}

	// create the overall structure
	struct snps_info *snps = (struct snps_info *)malloc(sizeof(struct snps_info));
	snps->info = malloc(sizeof(struct snps_t *)*number_of_lines);
	snps->length = number_of_lines;

	int index = 0;
	int control = 0;
	int columns = 0;
	char sep[] = "\t";
	char sep_intervals[] = ",";

	int line_numb = 1;
	sprintf(prev_chr, "init");
	idx_chr = 0;

	while (fgets(line, sizeof(line), file) != NULL) {
		if (isspace(line[0]) || line[0] == '#') {
			continue;
		}

		if (control == 0) {
			// count the number of columns
			for (i = 0; i < strlen(line); i++) {
				if (strncmp(&line[i], sep, 1) == 0) {
					columns++;
				}
			}
			columns++;
			control = 1;
		}

		// tokenize a line
		char *str_tokens[columns];
		char *pch;
		pch = strtok(line, "\t");
		i = 0;
		while (pch != NULL) {
			str_tokens[i++] = pch;
			pch = strtok(NULL, "\t");
		}

		if (i < 5) {
			fprintf(stderr, "ERROR: at line %d the number of columns is not correct (at least 3 columns required).\n", line_numb);
			exit(1);
		}

		struct snps_t *current_elem = (struct snps_t *)malloc(sizeof(struct snps_t));
		int size;
		current_elem->chr = (char*)malloc(strlen(str_tokens[0]) + 1);
		strcpy(current_elem->chr, str_tokens[0]);
		current_elem->pos = strtol(str_tokens[1], &pch, 10);
		// check correct conversion
		sprintf(stmp, "%u", current_elem->pos);
		if (strncmp(stmp, str_tokens[1], strlen(stmp)) != 0) {
			fprintf(stderr, "ERROR: genomic position at line %d is not valid.\n", line_numb);
			exit(1);
		}

		current_elem->rsid = (char*)malloc(strlen(str_tokens[2]) + 1);
		strcpy(current_elem->rsid, str_tokens[2]);
		current_elem->ref = (char*)malloc(strlen(str_tokens[3]) + 1);
		strcpy(current_elem->ref, str_tokens[3]);
		current_elem->alt = (char*)malloc(strlen(str_tokens[4]) + 1);
		strcpy(current_elem->alt, str_tokens[4]);

		// check ordering
		if (strcmp(current_elem->chr, prev_chr) != 0) {
			i = 0;
			while (VCF_CHR[i] != NULL) {
				if (strcmp(VCF_CHR[i], current_elem->chr) == 0) {
					fprintf(stderr, "ERROR: chromosomes are not ordered (line %d).\n", line_numb);
					exit(1);
				}
				i++;
			}
			sprintf(prev_chr, "%s", current_elem->chr);
			VCF_CHR[idx_chr] = (char *)malloc(sizeof(char) * strlen(current_elem->chr) + 1);
			strcpy(VCF_CHR[idx_chr], current_elem->chr);
			subSlash(VCF_CHR[idx_chr]);
			idx_chr++;
			prev_pos = 0;
		} else {
			if (current_elem->pos <= prev_pos) {
				fprintf(stderr, "ERROR: entries are not positionally ordered (line %d).\n", line_numb);
				exit(1);
			}

			prev_pos = current_elem->pos;
		}

		if (current_elem->pos < 0) {
			fprintf(stderr, "ERROR: position at line %d should be positive.\n", line_numb);
			exit(1);
		}
		if (strcmp(current_elem->ref, strA) != 0 && strcmp(current_elem->ref, strC) != 0 && strcmp(current_elem->ref, strG) != 0 && strcmp(current_elem->ref, strT) != 0) {
			fprintf(stderr, "ERROR: reference position at line %d is not a single base equal to A, C, G or T.\n", line_numb);
			exit(1);
		}
		if (strcmp(current_elem->alt, strA) != 0 && strcmp(current_elem->alt, strC) != 0 && strcmp(current_elem->alt, strG) != 0 && strcmp(current_elem->alt, strT) != 0) {
			fprintf(stderr, "ERROR: alternative position at line %d is not a single base equal to A, C, G or T.\n", line_numb);
			exit(1);
		}

		snps->info[index] = current_elem;
		index++;
		line_numb++;
	}

	return (snps);
}


///////////////////////////////////////////////////////////
// Print functions
///////////////////////////////////////////////////////////

void printTargetRegionSNVsPileup(FILE *outfileSNPs, FILE *outfileSNVs, FILE *outfileALL, FILE *outfileDUP, struct target_info *target_regions,
                                 struct snps_info *snps, struct input_args *arguments, int indexInit, int indexEnd)
{
	int i, r, c, alt, altG, ref, cov, covG, printID, postmp, ctrl;
	float af, afG;
	int j = 0;
	char *alt_base = (char*)malloc(2 * sizeof(char));
	char genotype[5];
	double z, pval;

	if (arguments->mode == 0 || arguments->mode == 1 || arguments->mode == 2) {
		if (arguments->genotype == 0) {
			fprintf(outfileSNPs, "chr\tpos\trsid\tref\talt\tA\tC\tG\tT\taf\tcov\n");
		} else {
			fprintf(outfileSNPs, "chr\tpos\trsid\tref\talt\tA\tC\tG\tT\taf\tcov\tgenotype\n");
		}
	}
	if (arguments->mode == 0 || arguments->mode == 1) {
		fprintf(outfileSNVs, "chr\tpos\tref\talt\tA\tC\tG\tT\taf\tcov");
		if (arguments->strand_bias == 1) {
			fprintf(outfileSNVs, "\tArs\tCrs\tGrs\tTrs\n");
		} else {
			fprintf(outfileSNVs, "\n");
		}
	}
	if (arguments->mode == 5) {
		if (arguments->strand_bias == 1) {
			fprintf(outfileSNVs, "chr\tpos\tref\talt\tA\tC\tG\tT\taf\tcov\tArs\tCrs\tGrs\tTrs\trsid\n");
		} else {
			fprintf(outfileSNVs, "chr\tpos\tref\talt\tA\tC\tG\tT\taf\tcov\trsid\n");
		}
	}

	if (arguments->mode == 1 || arguments->mode == 4)
		if (arguments->strand_bias == 1) {
			fprintf(outfileALL, "chr\tpos\tref\tA\tC\tG\tT\taf\tcov\tArs\tCrs\tGrs\tTrs\n");
		} else {
			fprintf(outfileALL, "chr\tpos\tref\tA\tC\tG\tT\taf\tcov\n");
		}

	if (arguments->mode == 5) {
		fprintf(outfileALL, "chr\tpos\tref\tA\tC\tG\tT\taf\tcov\trsid\n");
	}

	if (arguments->mode == 6) {
		fprintf(outfileALL, "chr\tpos\tref\tcov\tCountA\tFracA\tStrandA\tCountC\tFracC\tStrandC\tCountG\tFracG\tStrandG\tCountT\tFracT\tStrandT\n");
	}

	printID = 0;


	// Buffer for pileup writing
	char file_buffer[CHUNK_SIZE + 1024] ;
	int buffer_count = 0 ;
	c = 0;

	for (r = indexInit; r < indexEnd; r++) {
		i = 0;
		while (i < target_regions->info[r]->to - target_regions->info[r]->from + 1) {
			if (arguments->mode == 5) {
				covG = getSum(target_regions->info[r]->rdata, i);
				altG = getAlternativeSum(target_regions->info[r]->rdata, &(target_regions->info[r]->sequence[i]), i);
				afG = 0;
				if (covG > 0) {
					afG = ((float)altG) / ((float)covG);
				}

				fprintf(outfileALL, "%s\t%u\t%c\t%d\t%d\t%d\t%d\t%.6f\t%d\t",
				        target_regions->info[r]->chr,
				        target_regions->info[r]->from + i,
				        target_regions->info[r]->sequence[i],
				        target_regions->info[r]->rdata->positions[i].A,
				        target_regions->info[r]->rdata->positions[i].C,
				        target_regions->info[r]->rdata->positions[i].G,
				        target_regions->info[r]->rdata->positions[i].T, afG, covG);

				printID = 0;
				if (CheckAlt(target_regions->info[r], i) > 0) {
					ref = getBaseCount(target_regions->info[r]->rdata, &(target_regions->info[r]->sequence[i]), i);
					alt = FindAlternative(target_regions->info[r]->rdata, &(target_regions->info[r]->sequence[i]), i, alt_base);
					cov = alt + ref;
					af = 0;

					if (cov > 0) {
						af = ((float)alt) / ((float)cov);
					}

					if (cov >= arguments->mdc) {
						printID = 1;
						fprintf(outfileSNVs, "%s\t%u\t%c\t%s\t%d\t%d\t%d\t%d\t%.6f\t%d\t",
						        target_regions->info[r]->chr,
						        target_regions->info[r]->from + i,
						        target_regions->info[r]->sequence[i],
						        alt_base,
						        target_regions->info[r]->rdata->positions[i].A,
						        target_regions->info[r]->rdata->positions[i].C,
						        target_regions->info[r]->rdata->positions[i].G,
						        target_regions->info[r]->rdata->positions[i].T,
						        af, cov);
						if (arguments->strand_bias == 1) {
							fprintf(outfileSNVs, "%d\t%d\t%d\t%d\t",
							        target_regions->info[r]->rdata->positions[i].Asb,
							        target_regions->info[r]->rdata->positions[i].Csb,
							        target_regions->info[r]->rdata->positions[i].Gsb,
							        target_regions->info[r]->rdata->positions[i].Tsb);
						}
					}
				}
			}

			ctrl = 0;

			if (snps != NULL) {
				if (j < (snps->length - 1)) {
					while (getChrPos(target_regions->info[r]->chr) > getChrPos(snps->info[j]->chr) ||
					        (strcmp(target_regions->info[r]->chr, snps->info[j]->chr) == 0 &&
					         (i + target_regions->info[r]->from) > snps->info[j]->pos)) {
						j++;
						if (j == (snps->length) - 1) {
							break;
						}
					}
				}

				if (strcmp(target_regions->info[r]->chr, snps->info[j]->chr) == 0 &&
				        (i + target_regions->info[r]->from) == snps->info[j]->pos) {
					if (arguments->mode == 5) {
						fprintf(outfileALL, "%s\n", snps->info[j]->rsid);
						if (printID == 1) {
							fprintf(outfileSNVs, "%s\n", snps->info[j]->rsid);
						}
					}

					alt = getBaseCount(target_regions->info[r]->rdata, snps->info[j]->alt, i);
					ref = getBaseCount(target_regions->info[r]->rdata, snps->info[j]->ref, i);
					cov = alt + ref;

					af = 0;
					if (cov > 0) {
						af = ((float)alt) / ((float)cov);
					}

					if (cov >= arguments->mdc && (arguments->mode == 0 || arguments->mode == 1 || arguments->mode == 2)) {
						if (arguments->genotype == 1) {
							if (af < 0.2) {
								sprintf(genotype, "0/0");
							}
							if (af >= 0.2 && af <= 0.8) {
								sprintf(genotype, "0/1");
							}
							if (af > 0.8) {
								sprintf(genotype, "1/1");
							}
						} else if (arguments->genotype == 2) {
							z = ((ref / (ref + alt)) - 0.55) / sqrt((0.55 * 0.45) / (ref + alt));
							if (z < 0.) {
								z = z * (-1.0);
							}
							pval = 2 * (1 - cdf(z, 0, 1));
							if (pval <= 0.01 && ref > alt) {
								sprintf(genotype, "0/0");
							}
							if (pval <= 0.01 && ref < alt) {
								sprintf(genotype, "1/1");
							}
							if (pval > 0.01) {
								sprintf(genotype, "0/1");
							}
						}
						fprintf(outfileSNPs, "%s\t%u\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%.6f\t%d",
						        target_regions->info[r]->chr,
						        target_regions->info[r]->from + i,
						        snps->info[j]->rsid,
						        snps->info[j]->ref,
						        snps->info[j]->alt,
						        target_regions->info[r]->rdata->positions[i].A,
						        target_regions->info[r]->rdata->positions[i].C,
						        target_regions->info[r]->rdata->positions[i].G,
						        target_regions->info[r]->rdata->positions[i].T,
						        af, cov);
						if (arguments->genotype > 0) {
							fprintf(outfileSNPs, "\t%s\n", genotype);
						} else {
							fprintf(outfileSNPs, "\n");
						}
					}
					j++;
					if (j >= snps->length) {
						j = snps->length - 1;
					}
				} else {
					ctrl = 1;
				}
			} else {
				ctrl = 1;
			}


			if (ctrl == 1) {
				if (arguments->mode == 5) {
					fprintf(outfileALL, "\n");
					if (printID == 1) {
						fprintf(outfileSNVs, "\n");
					}
				}

				if (arguments->mode == 1 || arguments->mode == 4) {
					covG = getSum(target_regions->info[r]->rdata, i);
					altG = getAlternativeSum(target_regions->info[r]->rdata, &(target_regions->info[r]->sequence[i]), i);
					afG = 0;
					if (covG > 0) {
						afG = ((float)altG) / ((float)covG);
					}

					buffer_count += sprintf(&file_buffer[buffer_count], "%s\t%u\t%c\t%d\t%d\t%d\t%d\t%.6f\t%d",
					                        target_regions->info[r]->chr,
					                        target_regions->info[r]->from + i,
					                        target_regions->info[r]->sequence[i],
					                        target_regions->info[r]->rdata->positions[i].A,
					                        target_regions->info[r]->rdata->positions[i].C,
					                        target_regions->info[r]->rdata->positions[i].G,
					                        target_regions->info[r]->rdata->positions[i].T, afG, covG);

					if (arguments->strand_bias == 1) {
						buffer_count += sprintf(&file_buffer[buffer_count], "\t%d\t%d\t%d\t%d\n",
						                        target_regions->info[r]->rdata->positions[i].Asb,
						                        target_regions->info[r]->rdata->positions[i].Csb,
						                        target_regions->info[r]->rdata->positions[i].Gsb,
						                        target_regions->info[r]->rdata->positions[i].Tsb);
					} else {
						buffer_count += sprintf(&file_buffer[buffer_count], "\n");
					}
					// if the chunk is big enough, write it.
					if (buffer_count >= CHUNK_SIZE) {
						fwrite(file_buffer, CHUNK_SIZE, 1, outfileALL);
						buffer_count -= CHUNK_SIZE;
						memcpy(file_buffer, &file_buffer[CHUNK_SIZE], buffer_count);
					}
				}

				if (arguments->mode == 6) {
					covG = getSum(target_regions->info[r]->rdata, i);
					int del = target_regions->info[r]->rdata->positions[i].del;
					altG = getAlternativeSum(target_regions->info[r]->rdata, &(target_regions->info[r]->sequence[i]), i);
					afG = 0;
					if (covG > 0) {
						afG = ((float)altG) / ((float)covG);
					}

					double FracA, StrandA;
					if (target_regions->info[r]->rdata->positions[i].A) {
						StrandA = (target_regions->info[r]->rdata->positions[i].A - target_regions->info[r]->rdata->positions[i].Asb) / (double) target_regions->info[r]->rdata->positions[i].A;
					} else {
						StrandA = 0.0;
					}

					if (covG) {
						FracA = target_regions->info[r]->rdata->positions[i].A / (double) covG;
					} else {
						FracA = 0.0;
					}

					double FracC, StrandC;
					if (target_regions->info[r]->rdata->positions[i].C) {
						StrandC = (target_regions->info[r]->rdata->positions[i].C - target_regions->info[r]->rdata->positions[i].Csb) / (double) target_regions->info[r]->rdata->positions[i].C;
					} else {
						StrandC = 0.0;
					}

					if (covG) {
						FracC = target_regions->info[r]->rdata->positions[i].C / (double) covG;
					} else {
						FracC = 0.0;
					}

					double FracG, StrandG;
					if (target_regions->info[r]->rdata->positions[i].G) {
						StrandG = (target_regions->info[r]->rdata->positions[i].G - target_regions->info[r]->rdata->positions[i].Gsb) / (double) target_regions->info[r]->rdata->positions[i].G;
					} else {
						StrandG = 0.0;
					}

					if (covG) {
						FracG = target_regions->info[r]->rdata->positions[i].G / (double) covG;
					} else {
						FracG = 0.0;
					}

					double FracT, StrandT;
					if (target_regions->info[r]->rdata->positions[i].T) {
						StrandT = (target_regions->info[r]->rdata->positions[i].T - target_regions->info[r]->rdata->positions[i].Tsb) / (double) target_regions->info[r]->rdata->positions[i].T;
					} else {
						StrandT = 0.0;
					}

					if (covG) {
						FracT = target_regions->info[r]->rdata->positions[i].T / (double) covG;
					} else {
						FracT = 0.0;
					}
					// chr\tpos\tref\tcov\tCountA\tFracA\tStrandA\tCountC\tFracC\tStrandC\tCountG\tFracG\tStrandG\tCountT\tFracT\tStrandT
					
					buffer_count += sprintf(&file_buffer[buffer_count], "%s\t%u\t%c\t%d\t%d\t%.4f\t%.2f\t%d\t%.4f\t%.2f\t%d\t%.4f\t%.2f\t%d\t%.4f\t%.2f\n",
					                        target_regions->info[r]->chr,
					                        target_regions->info[r]->from + i,
					                        target_regions->info[r]->sequence[i],
					                        covG + del,
					                        target_regions->info[r]->rdata->positions[i].A,
					                        FracA,
					                        StrandA,
					                        target_regions->info[r]->rdata->positions[i].C,
					                        FracC,
					                        StrandC,
					                        target_regions->info[r]->rdata->positions[i].G,
					                        FracG,
					                        StrandG,
					                        target_regions->info[r]->rdata->positions[i].T,
					                        FracT,
					                        StrandT);

					// if the chunk is big enough, write it.
					if (buffer_count >= CHUNK_SIZE) {
						fwrite(file_buffer, CHUNK_SIZE, 1, outfileALL);
						buffer_count -= CHUNK_SIZE;
						memcpy(file_buffer, &file_buffer[CHUNK_SIZE], buffer_count);
					}
				}

				if (arguments->mode == 0 || arguments->mode == 1) {
					if (CheckAlt(target_regions->info[r], i) > 0) {
						ref = getBaseCount(target_regions->info[r]->rdata, &(target_regions->info[r]->sequence[i]), i);
						alt = FindAlternative(target_regions->info[r]->rdata, &(target_regions->info[r]->sequence[i]), i, alt_base);
						cov = alt + ref;
						af = 0;

						if (cov > 0) {
							af = ((float)alt) / ((float)cov);
						}

						if (cov >= arguments->mdc) {
							fprintf(outfileSNVs, "%s\t%u\t%c\t%s\t%d\t%d\t%d\t%d\t%.6f\t%d",
							        target_regions->info[r]->chr,
							        target_regions->info[r]->from + i,
							        target_regions->info[r]->sequence[i],
							        alt_base,
							        target_regions->info[r]->rdata->positions[i].A,
							        target_regions->info[r]->rdata->positions[i].C,
							        target_regions->info[r]->rdata->positions[i].G,
							        target_regions->info[r]->rdata->positions[i].T,
							        af, cov);
							if (arguments->strand_bias == 1) {
								fprintf(outfileSNVs, "\t%d\t%d\t%d\t%d\n",
								        target_regions->info[r]->rdata->positions[i].Asb,
								        target_regions->info[r]->rdata->positions[i].Csb,
								        target_regions->info[r]->rdata->positions[i].Gsb,
								        target_regions->info[r]->rdata->positions[i].Tsb);
							} else {
								fprintf(outfileSNVs, "\n");
							}
						}

					}
				}
			}


			i++;
		}
	}

	if (buffer_count > 0) {
		fwrite(file_buffer, 1, buffer_count, outfileALL);
	}

}

void printTargetRegionRC(FILE *outfile, struct target_t *elem)
{
	fprintf(outfile, "%s\t%u\t%u\t%d\t%d\t%.2f\t%.2f\t%.2f\n", elem->chr, elem->from, elem->to, elem->from_sel, elem->to_sel, elem->read_count_global, elem->read_count, elem->gc);
}

void printDUPLookupTable(struct lookup_dup *table)
{
	int i;
	for (i = 0; i < table->length; i++) {
		fprintf(stdout, "%d\t%d\t%d\n", table->covs_down[i], table->covs_up[i], table->thresholds[i]);
	}
}

///////////////////////////////////////////////////////////
// Multi-threaded pileup functions
///////////////////////////////////////////////////////////

struct args_thread {
	int start;
	int end;
	struct lookup_dup *duptable;
	struct target_info *target_regions;
	char bam[1000];
	char fasta[1000];
	struct input_args *arguments;
};

void *PileUp(void *args)
{
	struct args_thread *foo = (struct args_thread *)args;
	int i, r, ref, len, length, count, iter, hash_res, hash_res1, error, ll;
	char s[200];
	char stmp[200];
	struct region_data *tmp;
	bam_plbuf_t *buf;
	faidx_t *fasta;

	map_t *hmap;
	map_t *hmap_dups;
	dedup_struct_t* value;
	dup_struct_t* dup_value;
	char coords[KEY_MAX_LENGTH];
	fetch_reads_t *buff_data;

	samfile_t *in = samopen(foo->bam, "rb", 0);
	bam_index_t *idx = bam_index_load(foo->bam);

	fasta = fai_load(foo->fasta);

	for (r = foo->start; r <= foo->end; r++) {
		tmp = (struct region_data*)malloc(sizeof(struct region_data));
		tmp->beg = 0;
		tmp->end = 0x7fffffff;
		tmp->in = in;
		s[0] = '\0';
		sprintf(stmp, "%s", foo->target_regions->info[r]->chr);
		strcat(s, stmp);
		strcat(s, ":");
		sprintf(stmp, "%u", foo->target_regions->info[r]->from);
		strcat(s, stmp);
		strcat(s, "-");
		sprintf(stmp, "%u", foo->target_regions->info[r]->to);
		strcat(s, stmp);

		//fprintf(stderr,"%s\n",s);

		bam_parse_region(tmp->in->header, s, &ref, &(tmp->beg), &(tmp->end));

		if (ref < 0 || tmp->end < 0 || tmp->beg < 0) {
			fprintf(stderr, "ERROR: genomic region %s not compatible with BAM file.\n", s);
			exit(1);
		}

		foo->target_regions->info[r]->sequence = fai_fetch(fasta, s, &len);
		if (foo->target_regions->info[r]->sequence != NULL && len > 0) {
			length = strlen(foo->target_regions->info[r]->sequence);
			count = 0;
			while (count < length) {
				foo->target_regions->info[r]->sequence[count] = toupper(foo->target_regions->info[r]->sequence[count]);
				count++;
			}
		} else {
			fprintf(stderr, "ERROR: genomic region %s not compatible with FASTA file.\n", s);
			exit(1);
		}

		tmp->positions = (struct pos_pileup*)malloc(sizeof(struct pos_pileup) * (tmp->end - tmp->beg));
		for (i = 0; i < (tmp->end - tmp->beg); i++) {
			tmp->positions[i].A = tmp->positions[i].C = tmp->positions[i].G = tmp->positions[i].T = 0;
			tmp->positions[i].Asb = tmp->positions[i].Csb = tmp->positions[i].Gsb = tmp->positions[i].Tsb = 0;
			tmp->positions[i].del = 0;
		}
		tmp->duptable = foo->duptable;
		tmp->arguments = foo->arguments;

		buf = bam_plbuf_init(pileup_func, tmp);

		if (foo->arguments->dedup == 1) {
			hmap = hashmap_new();
			bam_fetch(tmp->in->x.bam, idx, ref, tmp->beg - tmp->arguments->dedup_window, tmp->end + tmp->arguments->dedup_window, hmap, fetch_func_dup);

			ll = hashmap_length(hmap);
			hmap_dups = hashmap_new();
			iter = 0;
			while ((hash_res = hashmap_iterate_external(hmap, iter, (void**)(&value))) != MAP_MISSING) {
				if (hash_res == MAP_OK) {
					getKey(value, coords);
					//fprintf(stderr,"%s %d\n",coords,value->isize);
					hash_res1 = hashmap_get(hmap_dups, coords, (void**)(&dup_value));
					if (hash_res1 == MAP_MISSING) {
						dup_value = malloc(sizeof(dup_struct_t));
						snprintf(dup_value->key_string, KEY_MAX_LENGTH, "%s", coords);
						dup_value->bp = value->bp;
						snprintf(dup_value->name, KEY_MAX_LENGTH, "%s", value->key_string);
						error = hashmap_put(hmap_dups, dup_value->key_string, dup_value);
						assert(error == MAP_OK);
					} else {
						if (value->bp > dup_value->bp) {
							dup_value->bp = value->bp;
							hashmap_remove(hmap, dup_value->name);
							snprintf(dup_value->name, KEY_MAX_LENGTH, "%s", value->key_string);
						} else {
							hashmap_remove(hmap, value->key_string);
						}
					}

				}
				iter++;
			}

			/*iter=0;
			while((hash_res=hashmap_iterate_external(hmap,iter,(void**)(&value)))!=MAP_MISSING)
			{
				if(hash_res==MAP_OK)
					fprintf(stderr,"ITER %s %d\n",value->key_string,value->isize);
				iter++;
			}*/
			hashmap_destroy(hmap_dups);
			buff_data = (fetch_reads_t *)malloc(sizeof(fetch_reads_t));
			buff_data->buf = buf;
			buff_data->hmap = hmap;
			bam_fetch(tmp->in->x.bam, idx, ref, tmp->beg, tmp->end, buff_data, fetch_func_dedup);
			bam_plbuf_push(0, buf);
			hashmap_destroy(hmap);
			free(buff_data);
		} else {
			bam_fetch(tmp->in->x.bam, idx, ref, tmp->beg, tmp->end, buf, fetch_func);
			bam_plbuf_push(0, buf);
		}

		bam_plbuf_destroy(buf);

		foo->target_regions->info[r]->rdata = tmp;

		if (foo->arguments->mode == 0 || foo->arguments->mode == 1 || foo->arguments->mode == 3) {
			computeRC(foo->arguments->region_perc, foo->target_regions->info[r]);
			computeGCRegion(foo->arguments->region_perc, foo->target_regions->info[r]);
		}

		if (foo->arguments->mode == 3) {
			foo->target_regions->info[r]->rdata = NULL;
			free(tmp->positions);
			free(tmp);
		}

	}

	fai_destroy(fasta);
	bam_index_destroy(idx);
	samclose(in);
}


///////////////////////////////////////////////////////////
// Main
///////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
	fprintf(stderr, "PaCBAM version 1.6.0\n");

	if (argc == 1) {
		printHelp();
		return 1;
	}

	char *tmp_string = NULL;
	char stmp[10000];
	struct lookup_dup* duptable = NULL;
	int i, j, k;

	printMessage("Load input parameters");
	struct input_args *arguments;
	arguments = getInputArgs(argv, argc);
	if (checkInputArgs(arguments) == 1) {
		return 1;
	}

	printArguments(arguments);

#ifdef _WIN32
	int result_code = mkdir(arguments->outdir);
#else
	mode_t process_mask = umask(0);
	int result_code = mkdir(arguments->outdir, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH);
	umask(process_mask);
#endif

	// Check correctness of BAM file
	samfile_t *in = samopen(arguments->bam, "rb", 0);
	if (in == 0) {
		fprintf(stderr, "ERROR: Fail to open BAM file.%s\n", arguments->bam);
		return 1;
	}
	samclose(in);

	// Check BAM index
	bam_index_t *idx = bam_index_load(arguments->bam); // load BAM index
	if (idx == 0) {
		fprintf(stderr, "ERROR: BAM indexing file is not available.\n");
		return 1;
	}
	bam_index_destroy(idx);

	// Check fasta file
	faidx_t *fasta = fai_load(arguments->fasta);
	if (fasta == 0) {
		return 1;
	}
	fai_destroy(fasta);

	// Init chr arrays
	BED_CHR = (char *)malloc(sizeof(char *)*MAX_CHR);
	VCF_CHR = (char *)malloc(sizeof(char *)*MAX_CHR);
	ORD_CHR = (char *)malloc(sizeof(char *)*MAX_CHR * 2);
	for (i = 0; i < MAX_CHR; i++) {
		BED_CHR[i] = VCF_CHR[i] = NULL;
	}
	for (i = 0; i < MAX_CHR * 2; i++) {
		ORD_CHR[i] = NULL;
	}

	printMessage("Load target regions");
	struct target_info* target_regions = loadTargetBed(arguments->bed);
	sprintf(stmp, "%d target regions loaded", target_regions->length);
	printMessage(stmp);
	struct snps_info* snps = NULL;
	printCHR(BED_CHR);
	if (arguments->mode == 0 || arguments->mode == 1 || arguments->mode == 2 || arguments->mode == 5) {
		printMessage("Load SNPs");
		snps = loadSNPs(arguments->vcf);
		sprintf(stmp, "%d snps loaded", snps->length);
		printMessage(stmp);
		printCHR(VCF_CHR);
		// Check BED vs VCF chr ordering
		i = k = 0;
		while (VCF_CHR[i] != NULL) {
			j = 0;
			while (BED_CHR[j] != NULL) {
				if (strcmp(VCF_CHR[i], BED_CHR[j]) == 0) {
					if (j < k) {
						fprintf(stderr, "ERROR: chromosomes specified in BED and VCF files have not the same order.\n");
						return (1);
					}
					k = j;
					break;
				}
				j++;
			}
			i++;
		}
		mergeBEDVCFCHRLists(VCF_CHR, BED_CHR, ORD_CHR);
	}

	if (arguments->duptablename != NULL) {
		printMessage("Load duplicates lookup table");
		duptable = loadDUPLookupTable(arguments->duptablename);
	}
	sprintf(stmp, "Compute pileup (Initialized %d threads)", arguments->cores);
	printMessage(stmp);
	pthread_t threads[arguments->cores];
	struct args_thread args[arguments->cores];

	int regions_per_core = ceil(target_regions->length / arguments->cores) + 1;

	i = 0;
	while (i < arguments->cores) {
		args[i].start = i * regions_per_core;
		args[i].end = (i + 1) * regions_per_core - 1;
		args[i].target_regions = target_regions;
		args[i].arguments = arguments;
		args[i].duptable = duptable;
		sprintf(args[i].bam, "%s", arguments->bam);
		sprintf(args[i].fasta, "%s", arguments->fasta);

		if (args[i].end >= (target_regions->length - 1)) {
			args[i].end = target_regions->length - 1;
		}

		pthread_create(&threads[i], NULL, PileUp, (void*)(&args[i]));

		i++;
	}

	for (i = 0; i < arguments->cores; i++) {
		pthread_join(threads[i], NULL);
	}

	// BAM file name
	FILE *outfile, *outfileSNPs, *outfileSNVs, *outfileALL, *outfileREAD, *outfileDUP;
	outfile = outfileSNPs = outfileSNVs = outfileALL = outfileREAD = outfileDUP = NULL;
	char *outfile_name = NULL;
	int slash;

	if (tmp_string != NULL) {
		free(tmp_string);
	}
	slash = lastSlash(arguments->bam) + 1;
	if (strncmp(arguments->bam + (strlen(arguments->bam) - 4), ".bam", 4) == 0) {
		tmp_string = (char *)malloc(strlen(arguments->bam) - slash - 3);
		strncpy(tmp_string, arguments->bam + slash, strlen(arguments->bam) - slash - 4);
		tmp_string[strlen(arguments->bam) - slash - 4] = '\0';
	} else {
		tmp_string = (char *)malloc(strlen(arguments->bam) - slash + 1);
		strcpy(tmp_string, arguments->bam + slash);
	}

	chdir(arguments->outdir);

	if (arguments->mode == 0 || arguments->mode == 1 || arguments->mode == 2 || arguments->mode == 4 || arguments->mode == 5 || arguments->mode == 6) {
		// Print target regions positions
		sprintf(stmp, "Output single base pileup statistics files in folder %s", arguments->outdir);
		printMessage(stmp);

		if (arguments->mode == 0 || arguments->mode == 1 || arguments->mode == 2) {
			if (outfile_name != NULL) {
				free(outfile_name);
			}
			outfile_name = (char*)malloc(strlen(tmp_string) + 6);
			sprintf(outfile_name, "%s.snps", tmp_string);
			outfileSNPs = fopen(outfile_name, "w");
		}

		if (arguments->mode == 0 || arguments->mode == 1 || arguments->mode == 5) {
			if (outfile_name != NULL) {
				free(outfile_name);
			}
			outfile_name = (char*)malloc(strlen(tmp_string) + 6);
			sprintf(outfile_name, "%s.pabs", tmp_string);
			outfileSNVs = fopen(outfile_name, "w");
		}
		if (arguments->mode == 1 || arguments->mode == 4 || arguments->mode == 5 || arguments->mode == 6) {
			if (outfile_name != NULL) {
				free(outfile_name);
			}
			outfile_name = (char*)malloc(strlen(tmp_string) + 8);
			sprintf(outfile_name, "%s.pileup", tmp_string);
			outfileALL = fopen(outfile_name, "w");
		}

		printTargetRegionSNVsPileup(outfileSNPs, outfileSNVs, outfileALL, outfileDUP, target_regions, snps, arguments, 0, target_regions->length);
		if (outfileSNPs != NULL) {
			fclose(outfileSNPs);
		}
		if (outfileSNVs != NULL) {
			fclose(outfileSNVs);
		}
		if (outfileALL != NULL) {
			fclose(outfileALL);
		}
		if (outfileDUP != NULL) {
			fclose(outfileDUP);
		}
	}

	if (arguments->mode == 0 || arguments->mode == 1 || arguments->mode == 3) {
		// Print target regions read count
		if (outfile_name != NULL) {
			free(outfile_name);
		}
		outfile_name = (char*)malloc(strlen(tmp_string) + 4);
		sprintf(outfile_name, "%s.rc", tmp_string);

		sprintf(stmp, "Output regions statistics file in folder %s.", arguments->outdir);
		printMessage(stmp);
		outfile = fopen(outfile_name, "w");
		fprintf(outfile, "chr\tfrom\tto\tfromS\ttoS\trc\trcS\tgc\n");
		for (i = 0; i < target_regions->length; i++) {
			printTargetRegionRC(outfile, target_regions->info[i]);
		}
		fclose(outfile);
	}

	printMessage("Computation end.");
	return 0;
}


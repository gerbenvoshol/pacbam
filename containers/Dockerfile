FROM debian:buster-slim AS build

# ARG pkg_version

WORKDIR /build

RUN apt-get update                                               && \
    apt-get install -y gcc                                          \
                       g++                                          \
                       make                                         \
                       git                                          \
                       zlib1g-dev                                   \
                       libncurses-dev                               \
                                                                 && \
    git clone https://CibioBCG@bitbucket.org/CibioBCG/pacbam.git && \
    cd pacbam                                                    && \
    cd include/samtools                                          && \
    make                                                         && \
    cd ../..                                                     && \
    rm -rf ./lib/linux64/libbam.a                                && \
    ln -s ../../include/samtools/libbam.a ./lib/linux64/         && \
    make -f Makefile.linux

FROM debian:buster-slim


RUN apt-get update                       && \
    apt-get install -y python2.7            \
                       python-matplotlib    \
                       python-numpy      && \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /build/pacbam/pacbam /usr/bin/
COPY --from=build /build/pacbam/pacbam_report.py /usr/bin/

CMD [ "/usr/bin/pacbam" ]
#!/bin/bash

ssh -o "StrictHostKeyChecking=no" $1 hostname
# ssh $1 "rm -rf resilientdb"
# ssh $1 "git clone https://github.com/p-shubham/resilientdb.git && cd resilientdb && git checkout HPC && cd deps && \ls | xargs -i tar -xvf {} && cd .. && mkdir obj && mkdir results"
rsync -a /Users/shubhampandey/Desktop/Projects/resilientdb $1:~/resilientdb
# ssh $1 "sudo apt-get -y update && sudo apt-get -y install libibverbs1 ibverbs-utils librdmacm1 rdmacm-utils libdapl2 ibsim-utils ibutils libcxgb3-1 libibmad5 libibumad3 libmlx4-1 libmthca1 libnes1 infiniband-diags mstflint opensm perftest srptools libibverbs-dev"
# ssh $1 "ibstat"
# ssh $1 "cd resilientdb && make -j32"
scp ifconfig.txt $1:~/resilientdb/
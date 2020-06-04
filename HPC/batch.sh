#!/bin/bash
#SBATCH --time=00:10:00
#SBATCH --nodes=17
#SBATCH --account=cpu-s2-moka_blox-0
#SBATCH --partition=cpu-s2-core-0
#SBATCH --mem=2G # Memory pool for all cores (see also --mem-per-cpu)
#SBATCH -o outs/out.txt # File to which STDOUT will be written
#SBATCH -e errs/err.txt # File to which STDERR will be written


ALLIP=$(srun -N 17 ./getIP)
echo "$ALLIP" > outs/hostandIPs.txt
while read line || [ -n "$line" ]; do
    for word in $line
    do
        if [[ $word == *"172"* ]]; then
            echo "$word" >> outs/ifconfig.txt
        fi
    done
done < outs/hostandIPs.txt

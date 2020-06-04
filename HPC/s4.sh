#!/bin/bash
#SBATCH --time=00:10:00
#SBATCH --nodelist=cpu-20
#SBATCH --account=cpu-s2-moka_blox-0
#SBATCH --partition=cpu-s2-core-0
#SBATCH --mem=2G # Memory pool for all cores (see also --mem-per-cpu)
#SBATCH -o out4.txt # File to which STDOUT will be written
#SBATCH -e err4.txt # File to which STDERR will be written

sbcast -f ifconfig.txt ifconfig.txt
sbcast -f config.h config.h

srun sleep 10
srun ./rundb -nid4
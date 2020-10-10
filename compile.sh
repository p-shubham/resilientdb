make clean
singularity exec resilient make -j32
cp rundb ~/resDB
cp runcl ~/resDB
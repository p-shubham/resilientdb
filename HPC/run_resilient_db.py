import os
import sys
cpu = [15,23,25,26,27,29,37,38]

def __main__(cpu,replica=4,client=1, username=''):
    ifconfig = create_ifconfig(cpu,replica,client)
    scripts = create_bash_scripts(cpu, replica, client)
    if ifconfig and scripts:
        command = ''
        for x in range(replica):
            command += 'sbatch s' + str(x) + '.sh && '
        for y in range(replica,replica+client):
            command += 'sbatch c' + str(y-replica) + '.sh'
        #print(command)
        stream = os.popen(command)
        stream = stream.read()
        print(stream)
        command = 'squeue -u' + username
        stream = os.popen(command)
        print(stream)
        
def create_ifconfig(cpu, replica, client):
    IP = []
    for index in range(replica+client):
        var = cpu[index]
        ip = "172.19.4."+str(var + 1)+"\n"
        IP.append(ip)
    PATH = './ifconfig.txt'
    f = open(PATH,'w')
    f.writelines(IP)
    f.close()
    return True

def create_bash_scripts(cpu, replica, client):
    for r in range(replica):
        lines = []
        lines.append("#!/bin/bash\n")
        lines.append("#SBATCH --time=01:00:00\n")
        lines.append("#SBATCH --nodelist=cpu-" + str(cpu[r]) + "\n")
        lines.append("#SBATCH --account=cpu-s2-moka_blox-0"+"\n")
        lines.append("#SBATCH --partition=cpu-s2-core-0"+"\n")
        lines.append("#SBATCH --reservation=cpu-s2-moka_blox-0_16"+"\n")
        lines.append("#SBATCH --mem=2G"+"\n")
        lines.append("#SBATCH --ntasks=1"+"\n")
        lines.append("#SBATCH --cpus-per-task=32"+"\n")
        lines.append("#SBATCH -o out" + str(r) + ".txt"+"\n")
        lines.append("#SBATCH -e err" + str(r) + ".txt"+"\n")
        lines.append("sbcast -f ./ifconfig.txt ifconfig.txt"+"\n")
        lines.append("sbcast -f ./config.h config.h"+"\n")
        lines.append("srun --nodelist=cpu-" + str(cpu[r]) + " sleep 10" + "\n")
        lines.append("srun --nodelist=cpu-" + str(cpu[r]) + " ./rundb -nid" + str(r)+"\n")
        PATH = './s' + str(r) + '.sh'
        f = open(PATH,'w')
        f.writelines(lines)
        f.close()

    for c in range(replica,replica + client):
        lines = []
        lines.append("#!/bin/bash\n")
        lines.append("#SBATCH --time=01:00:00\n")
        lines.append("#SBATCH --nodelist=cpu-" + str(cpu[c]) + "\n")
        lines.append("#SBATCH --account=cpu-s2-moka_blox-0"+"\n")
        lines.append("#SBATCH --partition=cpu-s2-core-0"+"\n")
        lines.append("#SBATCH --reservation=cpu-s2-moka_blox-0_16"+"\n")
        lines.append("#SBATCH --mem=2G"+"\n")
        lines.append("#SBATCH --ntasks=1"+"\n")
        lines.append("#SBATCH --cpus-per-task=32"+"\n")
        lines.append("#SBATCH -o out" + str(c) + ".txt"+"\n")
        lines.append("#SBATCH -e err" + str(c) + ".txt"+"\n")
        lines.append("sbcast -f ./ifconfig.txt ifconfig.txt"+"\n")
        lines.append("sbcast -f ./config.h config.h"+"\n")
        lines.append("srun --nodelist=cpu-" + str(cpu[c]) + " sleep 10" + "\n")
        lines.append("srun --nodelist=cpu-" + str(cpu[c]) + " ./runcl -nid" + str(c)+"\n")
        PATH = './c' + str(c-replica) + '.sh'
        f = open(PATH,'w')
        f.writelines(lines)
        f.close()
    return True

if(len(sys.argv) == 4):
    print("Remember: rundb & runcl must be complied with correct values in config.h")
    __main__(cpu,int(sys.argv[1]),int(sys.argv[2]),sys.argv[3])
else:
    print("Command line arguments absent or invalid, please retry. \n Format: python run_resilient_db <replica> <client> <username>")
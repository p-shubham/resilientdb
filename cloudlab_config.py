import subprocess
import threading

def executor(host, id):
    command = 'bash cloudlab_config.sh ' + host + ' ' + str(id)
    subprocess.call(command, shell=True)
    
class myThread (threading.Thread):
    def __init__(self, host, id):
        threading.Thread.__init__(self)
        self.host = host
        self.id = id
    def run(self):
        executor(self.host, id)

f = open("hosts", "r")
all_threads = []
for id, line in enumerate(f):
    all_threads.append(myThread(line, id))

for thread in all_threads:
    thread.start()
for thread in all_threads:
    thread.join()
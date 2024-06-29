import subprocess
import re
import matplotlib.pyplot as plt
import time

def run_command(command):
    result = subprocess.run(command, shell=True, text=True, capture_output=True)
    return result.stdout

def get_total_time_from_dmesg():
    dmesg_output = run_command("sudo dmesg | tail -n 20")
    match = re.search(r'Total time taken by all subscribers:\s+([\d\.]+)s', dmesg_output)
    if match:
        return float(match.group(1))
    return None

def get_overall_time_from_dmesg():
    dmesg_output = run_command("sudo dmesg | tail -n 20")
    match = re.search(r'Overall time taken by all subscribers:\s+([\d\.]+)s', dmesg_output)
    if match:
        return float(match.group(1))
    return None
subscribers_range = range(1, 1001)
total_times = []
overall_times = []

for subscribers in subscribers_range:
    print(f"Testing with {subscribers} publishers...")
    run_command(f"make load PUBLISHERS={subscribers} SUBSCRIBERS=500")
    run_command("sudo rmmod pubsub")
    total_time = get_total_time_from_dmesg()
    overall_time = get_overall_time_from_dmesg()
    print(total_time)
    print(overall_time)
    if total_time is not None and total_time<2000:
        total_times.append(total_time)
    else:
        total_times.append(0)
    if overall_time is not None:
        overall_times.append(overall_time)
    else:
        overall_times.append(0)

plt.figure(figsize=(10, 6))
plt.scatter(subscribers_range, total_times, color='purple', marker='+')
plt.title('Pub/Sub performance (Kthread impl)')
plt.xlabel('Publishers no.')
plt.ylabel('time (s)')

plt.savefig('kecho_concurrent_performance2.png')

plt.show()

plt.figure(figsize=(10, 6))
plt.scatter(subscribers_range, overall_times, color='purple', marker='+')
plt.title('Pub/Sub performance (Kthread impl)')
plt.xlabel('Publishers no.')
plt.ylabel('time (s)')

plt.savefig('kecho_concurrent_performance_test2.png')

plt.show()

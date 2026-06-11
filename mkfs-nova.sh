#!/bin/bash

## Inplace ##
#sudo modprobe nova metadata_csum=1 data_csum=1 data_parity=1 dram_struct_csum=1; sudo mount -t NOVA -o init /dev/pmem0 /mnt/test
#sudo modprobe nova metadata_csum=1 data_csum=1 data_parity=1; sudo mount -t NOVA -o init /dev/pmem0 /mnt/test
#sudo modprobe nova metadata_csum=1 data_csum=1; sudo mount -t NOVA -o init /dev/pmem0 /mnt/test
#sudo modprobe nova metadata_csum=1; sudo mount -t NOVA -o init /dev/pmem0 /mnt/test
#sudo modprobe nova; sudo mount -t NOVA -o init /dev/pmem0 /mnt/test

## Copy-on-write ##
#sudo modprobe nova metadata_csum=1 data_csum=1 data_parity=1 dram_struct_csum=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
#sudo modprobe nova metadata_csum=1 data_csum=1 data_parity=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
#sudo modprobe nova metadata_csum=1 data_csum=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

./linux-6.6.38/init_64_dwq_max_transfer.sh
./set_numa_online.sh 
#./set_numa_online.sh 1 0

./inner-mkfs.sh

#sudo rm -rf /lib/modules/$(uname -r)/kernel/fs/nova/nova.ko
#sudo cp ./module_nova-fortis/nova.ko /lib/modules/$(uname -r)/kernel/fs/nova/
##sudo modprobe nova metadata_csum=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
##sudo modprobe nova metadata_csum=1 data_csum=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
#sudo modprobe nova metadata_csum=1 data_csum=1 data_parity=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
##sudo modprobe nova metadata_csum=1 data_csum=1 dram_struct_csum=1 data_parity=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
##sudo modprobe nova metadata_csum=1; sudo mount -t NOVA -o init /dev/pmem0 /mnt/test
##sudo modprobe nova; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
#
#

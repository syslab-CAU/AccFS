sudo rm -rf /lib/modules/$(uname -r)/kernel/fs/nova/nova.ko
sudo cp ./module_nova-fortis/nova.ko /lib/modules/$(uname -r)/kernel/fs/nova/
#sudo modprobe nova metadata_csum=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
#sudo modprobe nova metadata_csum=1 data_csum=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
sudo modprobe nova metadata_csum=1 data_csum=1 data_parity=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
#sudo modprobe nova metadata_csum=1 data_csum=1 dram_struct_csum=1 data_parity=1; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test
#sudo modprobe nova metadata_csum=1; sudo mount -t NOVA -o init /dev/pmem0 /mnt/test
#sudo modprobe nova; sudo mount -t NOVA -o init,data_cow /dev/pmem0 /mnt/test



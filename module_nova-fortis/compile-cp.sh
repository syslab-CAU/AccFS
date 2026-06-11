#!/bin/bash

make -j12
#sudo cp nova.ko /lib/modules/6.6.38dsa/kernel/fs/nova/
sudo cp nova.ko /lib/modules/$(uname -r)/kernel/fs/nova/

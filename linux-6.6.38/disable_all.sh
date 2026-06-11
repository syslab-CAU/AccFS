# Disable prev wq and dev
for i in {0..7}
do
  accel-config disable-wq dsa0/wq0.$i
done
accel-config disable-device dsa0 

for i in {0..7}
do
  accel-config disable-wq dsa1/wq1.$i
done
accel-config disable-device dsa1 

for i in {0..7}
do
  accel-config disable-wq dsa2/wq2.$i
done
accel-config disable-device dsa2 

for i in {0..7}
do
  accel-config disable-wq dsa3/wq3.$i
done
accel-config disable-device dsa3 

for i in {0..7}
do
  accel-config disable-wq dsa4/wq4.$i
done
accel-config disable-device dsa4 

for i in {0..7}
do
  accel-config disable-wq dsa5/wq5.$i
done
accel-config disable-device dsa5 

for i in {0..7}
do
  accel-config disable-wq dsa6/wq6.$i
done
accel-config disable-device dsa6 

for i in {0..7}
do
  accel-config disable-wq dsa7/wq7.$i
done
accel-config disable-device dsa7 


echo "ls /sys/class/dma/ | wc -l"
ls /sys/class/dma/ | wc -l

echo "ls /sys/bus/dsa/devices | grep -e dsa! | wc -l"
ls /sys/bus/dsa/devices | grep -e dsa! | wc -l

#accel-config list | grep -e dsa -e wq -e kernel -e user -e dedicated -e shared

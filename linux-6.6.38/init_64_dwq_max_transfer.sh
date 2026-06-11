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



# Configure dsa0 related things
accel-config config-device dsa0
#accel-config config-group dsa0/group0.0
#accel-config config-group dsa0/group0.1
#accel-config config-group dsa0/group0.2
#accel-config config-group dsa0/group0.3

# Assign each engine to a separate group
accel-config config-engine dsa0/engine0.0 --group-id=0
accel-config config-engine dsa0/engine0.1 --group-id=1
accel-config config-engine dsa0/engine0.2 --group-id=2
accel-config config-engine dsa0/engine0.3 --group-id=3

# Configure dsa0 group0 wq0.0 single engine
accel-config config-wq dsa0/wq0.0 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app00 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group1 wq0.1 single engine
accel-config config-wq dsa0/wq0.1 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app01 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group2 wq0.2 single engine
accel-config config-wq dsa0/wq0.2 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app02 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group3 wq0.3 single engine
accel-config config-wq dsa0/wq0.3 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app03 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group0 wq0.4 shares engine
accel-config config-wq dsa0/wq0.4 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app04 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group1 wq0.5 shares engine
accel-config config-wq dsa0/wq0.5 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app05 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group2 wq0.6 shares engine
accel-config config-wq dsa0/wq0.6 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app06 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group3 wq0.7 shares engine
accel-config config-wq dsa0/wq0.7 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app07 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

accel-config enable-device dsa0
for i in {0..7}
do
  accel-config enable-wq dsa0/wq0.$i
done


# Configure dsa1 group1 wq1.0 single engine
accel-config config-device dsa1

# Assign each engine to a separate group
accel-config config-engine dsa1/engine1.0 --group-id=0
accel-config config-engine dsa1/engine1.1 --group-id=1
accel-config config-engine dsa1/engine1.2 --group-id=2
accel-config config-engine dsa1/engine1.3 --group-id=3

# Configure dsa0 group0 wq1.0 single engine
accel-config config-wq dsa1/wq1.0 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app10 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group1 wq1.1 single engine
accel-config config-wq dsa1/wq1.1 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app11 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group2 wq1.2 single engine
accel-config config-wq dsa1/wq1.2 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app12 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group3 wq1.3 single engine
accel-config config-wq dsa1/wq1.3 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app13 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group0 wq0.4 shares engine
accel-config config-wq dsa1/wq1.4 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app14 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group1 wq0.5 shares engine
accel-config config-wq dsa1/wq1.5 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app15 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group2 wq0.6 shares engine
accel-config config-wq dsa1/wq1.6 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app16 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

# Configure dsa0 group3 wq0.7 shares engine
accel-config config-wq dsa1/wq1.7 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app17 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

accel-config enable-device dsa1
for i in {0..7}
do
  accel-config enable-wq dsa1/wq1.$i
done


# Configure dsa2 
accel-config config-device dsa2

# Assign each engine to a separate group
accel-config config-engine dsa2/engine2.0 --group-id=0
accel-config config-engine dsa2/engine2.1 --group-id=1
accel-config config-engine dsa2/engine2.2 --group-id=2
accel-config config-engine dsa2/engine2.3 --group-id=3

accel-config config-wq dsa2/wq2.0 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app20 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa2/wq2.1 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app21 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa2/wq2.2 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app22 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa2/wq2.3 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app23 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa2/wq2.4 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app24 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa2/wq2.5 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app25 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa2/wq2.6 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app26 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa2/wq2.7 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app27 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

accel-config enable-device dsa2
for i in {0..7}
do
  accel-config enable-wq dsa2/wq2.$i
done

# Configure dsa3 
accel-config config-device dsa3

# Assign each engine to a separate group
accel-config config-engine dsa3/engine3.0 --group-id=0
accel-config config-engine dsa3/engine3.1 --group-id=1
accel-config config-engine dsa3/engine3.2 --group-id=2
accel-config config-engine dsa3/engine3.3 --group-id=3

accel-config config-wq dsa3/wq3.0 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app30 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa3/wq3.1 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app31 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa3/wq3.2 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app32 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa3/wq3.3 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app33 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa3/wq3.4 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app34 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa3/wq3.5 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app35 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa3/wq3.6 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app36 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa3/wq3.7 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app37 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

accel-config enable-device dsa3
for i in {0..7}
do
  accel-config enable-wq dsa3/wq3.$i
done

# Configure dsa4 
accel-config config-device dsa4

# Assign each engine to a separate group
accel-config config-engine dsa4/engine4.0 --group-id=0
accel-config config-engine dsa4/engine4.1 --group-id=1
accel-config config-engine dsa4/engine4.2 --group-id=2
accel-config config-engine dsa4/engine4.3 --group-id=3

accel-config config-wq dsa4/wq4.0 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app40 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa4/wq4.1 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app41 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa4/wq4.2 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app42 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa4/wq4.3 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app43 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa4/wq4.4 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app44 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa4/wq4.5 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app45 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa4/wq4.6 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app46 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa4/wq4.7 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app47 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

accel-config enable-device dsa4
for i in {0..7}
do
  accel-config enable-wq dsa4/wq4.$i
done

# Configure dsa5 
accel-config config-device dsa5

# Assign each engine to a separate group
accel-config config-engine dsa5/engine5.0 --group-id=0
accel-config config-engine dsa5/engine5.1 --group-id=1
accel-config config-engine dsa5/engine5.2 --group-id=2
accel-config config-engine dsa5/engine5.3 --group-id=3

accel-config config-wq dsa5/wq5.0 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app50 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa5/wq5.1 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app51 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa5/wq5.2 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app52 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa5/wq5.3 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app53 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa5/wq5.4 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app54 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa5/wq5.5 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app55 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa5/wq5.6 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app56 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa5/wq5.7 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app57 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

accel-config enable-device dsa5
for i in {0..7}
do
  accel-config enable-wq dsa5/wq5.$i
done

# Configure dsa6 
accel-config config-device dsa6

# Assign each engine to a separate group
accel-config config-engine dsa6/engine6.0 --group-id=0
accel-config config-engine dsa6/engine6.1 --group-id=1
accel-config config-engine dsa6/engine6.2 --group-id=2
accel-config config-engine dsa6/engine6.3 --group-id=3

accel-config config-wq dsa6/wq6.0 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app60 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa6/wq6.1 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app61 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa6/wq6.2 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app62 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa6/wq6.3 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app63 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa6/wq6.4 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app64 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa6/wq6.5 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app65 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa6/wq6.6 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app66 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa6/wq6.7 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app67 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

accel-config enable-device dsa6
for i in {0..7}
do
  accel-config enable-wq dsa6/wq6.$i
done


# Configure dsa7 
accel-config config-device dsa7

# Assign each engine to a separate group
accel-config config-engine dsa7/engine7.0 --group-id=0
accel-config config-engine dsa7/engine7.1 --group-id=1
accel-config config-engine dsa7/engine7.2 --group-id=2
accel-config config-engine dsa7/engine7.3 --group-id=3

accel-config config-wq dsa7/wq7.0 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app70 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa7/wq7.1 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app71 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa7/wq7.2 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app72 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa7/wq7.3 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app73 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa7/wq7.4 --group-id=0 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app74 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa7/wq7.5 --group-id=1 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app75 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa7/wq7.6 --group-id=2 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app76 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456
accel-config config-wq dsa7/wq7.7 --group-id=3 --wq-size=16 --priority=1 --block-on-fault=0 --type=kernel --name=app77 --mode=dedicated --max-batch-size=32 --max-transfer-size=268435456

accel-config enable-device dsa7
for i in {0..7}
do
  accel-config enable-wq dsa7/wq7.$i
done

echo "ls /sys/class/dma/ | wc -l"
ls /sys/class/dma/ | wc -l

obj-m += rum4linux.o

rum4linux-y := \
	src/rum4linux_core.o \
	src/rum4linux_hw.o \
	src/rum4linux_eeprom.o \
	src/rum4linux_fw.o \
	src/rum4linux_bbp.o \
	src/rum4linux_rf.o \
	src/rum4linux_tx.o \
	src/rum4linux_rx.o

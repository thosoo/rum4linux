obj-m += rum4linux.o

rum4linux-y := \
	src/dwa111_rum_main.o \
	src/dwa111_rum_hw.o \
	src/dwa111_rum_eeprom.o \
	src/dwa111_rum_fw.o \
	src/rum4linux_bbp.o \
	src/rum4linux_rf.o

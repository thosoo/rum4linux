obj-m += dwa111_rum.o

dwa111_rum-y := \
	src/dwa111_rum_main.o \
	src/dwa111_rum_hw.o \
	src/dwa111_rum_eeprom.o \
	src/dwa111_rum_fw.o

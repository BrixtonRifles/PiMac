# Bare-metal build (Circle). Run `make` from the project root instead.

CIRCLEHOME = ../deps/circle

CORE_OBJS = $(patsubst %.cpp,%.o,$(wildcard core/*.cpp))
HOST_OBJS = $(patsubst %.cpp,%.o,$(wildcard host/*.cpp))

OBJS = main.o kernel.o $(HOST_OBJS) $(CORE_OBJS)

LIBS = $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

EXTRACLEAN = core/*.o core/*.d host/*.o host/*.d

include $(CIRCLEHOME)/Rules.mk

INCLUDE += -I .

-include $(DEPS)

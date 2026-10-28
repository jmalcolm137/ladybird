# Build the LibGUI frontend against the Lagom LibGUI used by the Serenity
# Desktop Environment (libgui-wayland).
#
#   make                    build build/chrome/Ladybird
#   make -j                 build in parallel
#   make clean
#
# Override the framework location if it lives elsewhere:
#   make LAGOM=/opt/libgui-wayland/build/lagom SERENITY=/opt/libgui-wayland/serenity

ROOT := $(abspath $(CURDIR)/../..)
LIBGUI_WAYLAND_ROOT ?= $(ROOT)/../libgui-wayland
LAGOM ?= $(abspath $(LIBGUI_WAYLAND_ROOT)/build/lagom)
SERENITY ?= $(abspath $(LIBGUI_WAYLAND_ROOT)/serenity)
BIN_DIR := $(ROOT)/build/chrome
TARGET := $(BIN_DIR)/Ladybird

CXX ?= c++
CXXFLAGS ?= -std=c++26 -fno-exceptions -O2 -g -Wall -Wextra -Wno-literal-suffix
CPPFLAGS += \
	-I$(SERENITY) \
	-I$(SERENITY)/Userland \
	-I$(SERENITY)/Userland/Libraries \
	-I$(SERENITY)/Userland/Services \
	-I$(LAGOM) \
	-I$(LAGOM)/Userland/Libraries \
	-I$(LAGOM)/Userland/Services \
	-I$(LIBGUI_WAYLAND_ROOT)/compat \
	-I$(ROOT)

# libwindowserver.so is the in-process WindowServer implementation that turns a
# LibGUI window into a Wayland toplevel. Its historical name does not mean it is
# a service to start.
LDFLAGS += -L$(LAGOM)/lib -Wl,--no-as-needed -Wl,-rpath,$(LAGOM)/lib -Wl,-rpath,'$$ORIGIN/../lib'
LDLIBS += \
	-llagom-gui \
	-llagom-gfx \
	-llagom-core \
	-llagom-corebasic \
	-llagom-coreminimal \
	-llagom-ipc \
	-llagom-main \
	-llagom-ak \
	-llagom-filesystem \
	-lwindowserver \
	-lpthread

SOURCES := \
	BrowserTab.cpp \
	BrowserWindow.cpp \
	DownloadsWindow.cpp \
	EngineClient.cpp \
	WebContentView.cpp \
	main.cpp

OBJECTS := $(SOURCES:%.cpp=$(BIN_DIR)/%.o)

.PHONY: all clean
all: $(TARGET)

$(BIN_DIR):
	mkdir -p $@

$(BIN_DIR)/%.o: %.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(BIN_DIR)

-include $(OBJECTS:.o=.d)

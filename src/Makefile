# SOURCES = $(wildcard *.cpp)


all:  shomagick


DEPDIR = deps
$(shell mkdir -p $(DEPDIR) >/dev/null)

CXXFLAGS += -std=c++11
CXXFLAGS += -O3
CXXFLAGS += -g
CXXFLAGS += -Wall -Wextra -pedantic
CXXFLAGS += -Weffc++
CXXFLAGS += -Werror=reorder
CXXFLAGS += -Werror=return-type

INCLUDEFLAGS += -isystem /usr/include/eigen3
INCLUDEFLAGS += -isystem /usr/include/python2.7

CXXFLAGS += $(INCLUDEFLAGS)

OPENCV_LINK_FLAGS += -lopencv_core
OPENCV_LINK_FLAGS += -lopencv_highgui
OPENCV_LINK_FLAGS += -lopencv_imgcodecs
OPENCV_LINK_FLAGS += -lopencv_imgproc
OPENCV_LINK_FLAGS += -lopencv_videoio
OPENCV_LINK_FLAGS += -lopencv_ml
OPENCV_LINK_FLAGS += -lopencv_xfeatures2d
OPENCV_LINK_FLAGS += -lopencv_objdetect

LFLAGS += -lboost_filesystem
LFLAGS += -lopengv
LFLAGS += -lboost_python
LFLAGS += -lboost_system
LFLAGS += -lexiv2
LFLAGS += -lglog
LFLAGS += -lpthread
LFLAGS += -lceres
LFLAGS += -lcxsparse
LFLAGS += -lsuitesparseconfig
LFLAGS += -llapack 
LFLAGS += -lblas	
LFLAGS += -lcholmod
LFLAGS += `pkg-config --libs opencv`
LFLAGS += -g
LFLAGS += -fopenmp
LFLAGS += -lgfortran

SOURCES += stitch.cpp
SOURCES += kdtree.cpp
SOURCES += flightsession.cpp
SOURCES += shomatcher.cpp
SOURCES += RobustMatcher.cpp
SOURCES += shotracking.cpp
SOURCES += reconstructor.cpp
SOURCES += camera.cpp
SOURCES += reconstruction.cpp
SOURCES += multiview.cpp

OBJECTS = $(SOURCES:.cpp=.o)
DEPS = $(OBJECTS:.o=.d)

shomagick : $(OBJECTS)
	$(CXX) -o $@ $(OBJECTS) $(LFLAGS)

%.o : %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MF $(DEPDIR)/$*.d -c $< -o $@

clean:
	rm -f $(OBJECTS) $(addprefix $(DEPDIR)/, $(DEPS)) shomagick shomagick.exe


-include $(addprefix $(DEPDIR)/, $(DEPS))




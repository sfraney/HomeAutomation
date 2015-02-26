install : Gatewayd
	cp Gatewayd /usr/local/bin

Gatewayd : Gatewayd.c
	g++ Gatewayd.c rfm69.cpp -o Gatewayd -lwiringPi -lmosquitto -DRASPBERRY

#Gatewayd : Gatewayd.c Gateway.o
#	g++ Gatewayd.c Gateway.o -o Gatewayd -lwiringPi -lmosquitto -DRASPBERRY

Gateway : Gateway.c rfm69.cpp
	g++ Gateway.c rfm69.cpp -o Gateway -lwiringPi -lmosquitto -DRASPBERRY

Gateway.o : Gateway.c rfm69.cpp
	g++ -c Gateway.c rfm69.cpp -o Gateway.o -lwiringPi -lmosquitto -DRASPBERRY

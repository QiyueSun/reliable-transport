all: wSender wReceiver

wSender: wSender.cpp
	g++ -std=c++11 -o wSender wSender.cpp

wReceiver: wReceiver.cpp
	g++ -std=c++11 -o wReceiver wReceiver.cpp

clean:
	rm -f wReceiver wSender recv_log.txt sender_log.txt ./tmp/*

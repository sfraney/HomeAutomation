/*
  RFM69 Gateway RFM69 pushing the data to the mosquitto server
  by Alexandre Bouillot

  License:  CC-BY-SA, https://creativecommons.org/licenses/by-sa/2.0/
  Date:  10-23-2014
  File: Gateway.ino
  This sketch receives RFM wireless data and forwards it to Mosquitto relay

  sensorNode.sensorID,

  Modifications Needed:
  1)  Update encryption string "ENCRYPTKEY"
*/

/*
  RFM69 Pinout:
  MOSI = 11
  MISO = 12
  SCK = 13
  SS = 8
*/

//general --------------------------------
#define DEBUG 1
#if DEBUG
#define DEBUG1(expression)  fprintf(stderr, expression)
#define DEBUG2(expression, arg)  fprintf(stderr, expression, arg)
#define DEBUGLN1(expression)  
#ifdef DAEMON
#define LOG(...) do { syslog(LOG_INFO, __VA_ARGS__); } while (0)
#define LOG_E(...) do { syslog(LOG_ERR, __VA_ARGS__); } while (0)
#else
#define LOG(...) do { printf(__VA_ARGS__); } while (0)
#define LOG_E(...) do { printf(__VA_ARGS__); } while (0)
#endif //DAEMON
#else
#define DEBUG1(expression)
#define DEBUG2(expression, arg)
#define DEBUGLN1(expression)
#define LOG(...)
#define LOG_E(...)
#endif

//RFM69  ----------------------------------
#include "rfm69.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <vector>

#define NODEID        1    //unique for each node on same network
#define NETWORKID     10   //the same on all nodes that talk to each other
#define FREQUENCY   RF69_433MHZ
//#define FREQUENCY   RF69_868MHZ
//#define FREQUENCY   RF69_915MHZ
#define ENCRYPTKEY    "GRACEVICTAFRANEY" //exactly the same 16 characters/bytes on all nodes!
#define IS_RFM69HW       //uncomment only for RFM69HW! Leave out if you have RFM69W!
#define ACK_TIME      30 // max # of ms to wait for an ack

RFM69 *rfm69;
bool promiscuousMode = false; //set to 'true' to sniff all packets on the same network
byte ackCount=0;

// Mosquitto---------------
#include <mosquitto.h>

/* How many seconds the broker should wait between sending out
 * keep-alive messages. */
#define KEEPALIVE_SECONDS 60
/* Hostname and port for the MQTT broker. */
#define BROKER_HOSTNAME "localhost"
#define BROKER_PORT 1883

#define MQTT_CLIENT_ID "arduinoClient"
#define MQTT_RETRY 500

int sendMQTT = 0;

typedef struct {
  short nodeID;			//node ID (1xx, 2xx, 3xx);  1xx = basement, 2xx = main floor, 3xx = outside
  short sensorID;		//sensor ID (2, 3, 4, 5)
  unsigned long uptime_usl;	//uptime in ms
  float sens_dat_flt;		//sensor data?
  float batt_con_flt;		//battery condition?
} Payload;
Payload theData;

typedef struct {
  short nodeID;
  short sensorID;
  unsigned long uptime_usl;
  float sens_dat_flt;
  float batt_con_flt;
  int var4_int;
} SensorNode;
SensorNode sensorNode;

static void die(const char *msg);
static bool set_callbacks(struct mosquitto *m);
static bool connect(struct mosquitto *m);
static int run_loop(struct mosquitto *m);

static void MQTTSendInt(struct mosquitto * _client, int node, int sensor, int var, int val);
static void MQTTSendULong(struct mosquitto* _client, int node, int sensor, int var, unsigned long val);
static void MQTTSendFloat(struct mosquitto* _client, int node, int sensor, int var, float val);

static void uso(void) {
  fprintf(stderr, "Use:\n Simply use it without args :D\n");
  exit(1);
}

struct tracker_entry {
  short node;
  time_t last;

  tracker_entry(short n_) : node(n_) { }
};

int main(int argc, char* argv[]) {
  if (argc != 1) uso();
#ifdef DAEMON
  pid_t pid, sid;

  openlog("Gatewayd", 0, LOG_USER);

  pid = fork();
  if (pid < 0) {
    LOG_E("fork failed");
    exit(EXIT_FAILURE);
  }
  /* If we got a good PID, then
     we can exit the parent process. */
  if (pid > 0) {
    LOG("Child spawned, pid %d\n", pid);
    exit(EXIT_SUCCESS);
  }

  /* Change the file mode mask */
  umask(0);
                
  /* Open any logs here */        
                
  /* Create a new SID for the child process */
  sid = setsid();
  if (sid < 0) {
    LOG_E("setsid failed");
    exit(EXIT_FAILURE);
  }
        
  /* Change the current working directory */
  if ((chdir("/")) < 0) {
    LOG_E("chdir failed");
    exit(EXIT_FAILURE);
  }
        
  /* Close out the standard file descriptors */
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
#endif //DAEMON
  struct mosquitto *mqtt = mosquitto_new(MQTT_CLIENT_ID, true, null);
  if (mqtt == NULL) { die("mosquitto init() failure\n"); }

  if (!set_callbacks(mqtt)) { die("set_callbacks() failure\n"); }
  if (!connect(mqtt)) { die("connect() failure\n"); }

  //RFM69 ---------------------------
  rfm69 = new RFM69();
  rfm69->initialize(FREQUENCY,NODEID,NETWORKID);
#ifdef IS_RFM69HW
  rfm69->setHighPower(); //uncomment only for RFM69HW!
#endif
  rfm69->encrypt(ENCRYPTKEY);
  rfm69->promiscuous(promiscuousMode);
  LOG("Listening at %d Mhz...", FREQUENCY==RF69_433MHZ ? 433 : FREQUENCY==RF69_868MHZ ? 868 : 915);

  LOG("setup complete\n");

  return run_loop(mqtt);
}  // end of setup

/* Loop until it is explicitly halted or the network is lost, then clean up. */
static int run_loop(struct mosquitto *mqtt) {
  int res;
  std::vector<tracker_entry> tracker;
  time_t last_check = time(NULL);
  while(1) {
    time_t curr_time = time(NULL);
    //Initialize nodeID to invalid (for determining if ack should be requested)
    sensorNode.nodeID = -1;

    res = mosquitto_loop(mqtt, 1000, 1);

    if (rfm69->receiveDone()) {
      printf("[%d] ", rfm69->SENDERID);
      if (promiscuousMode) {
       	printf(" to [%d] ", rfm69->TARGETID);
      }
      
      for(int i = 0; i < rfm69->DATALEN; i++) {
       	printf("%x.", rfm69->DATA[i]);
      }
      printf("\n");
      
      if (rfm69->DATALEN != sizeof(Payload)) {
	LOG_E("Invalid payload received, not matching Payload struct! %d - %d\r\n", rfm69->DATALEN, sizeof(Payload));
      } else {
	theData = *(Payload*)rfm69->DATA; //assume radio.DATA actually contains our struct and not something else

	sensorNode.nodeID = theData.nodeID;
	sensorNode.sensorID = theData.sensorID;
	sensorNode.uptime_usl = theData.uptime_usl;
	sensorNode.sens_dat_flt = theData.sens_dat_flt;
	sensorNode.batt_con_flt = theData.batt_con_flt;
	sensorNode.var4_int = rfm69->RSSI;

	printf("Received Node ID = %d Device ID = %d Time = %d  RSSI = %d var2 = %f var3 = %f\n",
	    sensorNode.nodeID,
	    sensorNode.sensorID,
	    sensorNode.uptime_usl,
	    sensorNode.var4_int,
	    sensorNode.sens_dat_flt,
	    sensorNode.batt_con_flt
	    );
	sendMQTT = 1;

	//Update tracker
	//find existing entry
	std::vector<tracker_entry>::iterator t_it;
	for(t_it = tracker.begin();
	    t_it != tracker.end();
	    ++t_it) {
	  if(t_it->node == sensorNode.nodeID) {
	    break;
	  }
	}
	if(t_it == tracker.end()) {  //entry not found
	  printf("Expanding tracker to %i entries\n", tracker.size() + 1);
	  tracker_entry n_tracker(sensorNode.nodeID);
	  tracker.push_back(n_tracker);
	  LOG("New node %i added\n", sensorNode.nodeID);
	  t_it = tracker.end();
	  --t_it;
	}
	printf("Updating node %i after %.f seconds\n", t_it->node, difftime(curr_time, t_it->last));
	t_it->last = curr_time;
      }

      if (rfm69->ACK_REQUESTED) {
	byte theNodeID = rfm69->SENDERID;
	rfm69->sendACK();

	// When a node requests an ACK, respond to the ACK
	// and also send a packet requesting an ACK (every 3rd one only)
	// This way both TX/RX NODE functions are tested on 1 end at the GATEWAY
	if ((ackCount++%3==0) && (sensorNode.nodeID != -1)) {
	  printf(" Pinging node %u - ACK...", sensorNode.nodeID);
	  //sleep(3); //need this when sending right after reception .. ?
	  if (rfm69->sendWithRetry(theNodeID, "ACK TEST", 8, 0)) {// 0 = only 1 attempt, no retries
	    printf("success\n");
	  } else {
	    printf("FAILURE\n");
	  }
	}
      }//end if radio.ACK_REQESTED
    } //end if radio.receive

    if (sendMQTT == 1) {
      //send uptime_usl
      MQTTSendULong(mqtt, sensorNode.nodeID, sensorNode.sensorID, 1, sensorNode.uptime_usl);

      //send sens_dat_flt
      MQTTSendFloat(mqtt, sensorNode.nodeID, sensorNode.sensorID, 2, sensorNode.sens_dat_flt);

      //send batt_con_flt
      MQTTSendFloat(mqtt, sensorNode.nodeID, sensorNode.sensorID, 3, sensorNode.batt_con_flt);

      //send var4_int, RSSI
      MQTTSendInt(mqtt, sensorNode.nodeID, sensorNode.sensorID, 4, sensorNode.var4_int);

      sendMQTT = 0;
    }//end if sendMQTT

    //check if any nodes lost contact in the last minute
    if(difftime(curr_time, last_check) >= 60) {
      for(std::vector<tracker_entry>::iterator t_it = tracker.begin();
	  t_it != tracker.end(); ) {
	if(difftime(curr_time, t_it->last) > 60) {  //Lost contact
	  LOG_E("Lost contact with node %i\n", t_it->node);
	  t_it = tracker.erase(t_it);
	} else {
	  ++t_it;
	}
      }
    }
    //    sleep(1);
  }

  mosquitto_destroy(mqtt);
  (void)mosquitto_lib_cleanup();

  if (res == MOSQ_ERR_SUCCESS) {
    return 0;
  } else {
    return 1;
  }
}


static void MQTTSendInt(struct mosquitto * _client, int node, int sensor, int var, int val) {
  char buff_topic[6];
  char buff_message[7];

  sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
  sprintf(buff_message, "%04d%", val);
  mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
}

static void MQTTSendULong(struct mosquitto* _client, int node, int sensor, int var, unsigned long val) {
  char buff_topic[6];
  char buff_message[12];

  sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
  sprintf(buff_message, "%u", val);
  mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
}

static void MQTTSendFloat(struct mosquitto* _client, int node, int sensor, int var, float val) {
  char buff_topic[6];
  char buff_message[12];

  sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
  snprintf(buff_message, 12, "%f", val);

  mosquitto_publish(_client, 0, buff_topic, strlen(buff_message), buff_message, 0, false);
}

// Handing of Mosquitto messages
void callback(char* topic, byte* payload, unsigned int length) {
  // handle message arrived
  /* LOG("Mosquitto Callback\n"); */
}

/* Fail with an error message. */
static void die(const char *msg) {
  fprintf(stderr, "%s", msg);
  exit(1);
}

/* Connect to the network. */
static bool connect(struct mosquitto *m) {
  int res = mosquitto_connect(m, BROKER_HOSTNAME, BROKER_PORT, KEEPALIVE_SECONDS);
  return res == MOSQ_ERR_SUCCESS;
}

/* Callback for successful connection: add subscriptions. */
static void on_connect(struct mosquitto *m, void *udata, int res) {
  if (res == 0) {             /* success */
    LOG("Connect succeed\n");
  } else {
    LOG_E("Connection Refused");
    die("connection refused\n");
  }
}

/* Handle a message that just arrived via one of the subscriptions. */
static void on_message(struct mosquitto *m, void *udata,
		       const struct mosquitto_message *msg) {
  if (msg == NULL) { return; }
  /* LOG("-- got message @ %s: (%d, QoS %d, %s) '%s'\n", */
  /*     msg->topic, msg->payloadlen, msg->qos, msg->retain ? "R" : "!r", */
  /*     msg->payload); */
}

/* A message was successfully published. */
static void on_publish(struct mosquitto *m, void *udata, int m_id) {
  /* LOG("-- published successfully\n"); */
}

/* Successful subscription hook. */
static void on_subscribe(struct mosquitto *m, void *udata, int mid,
			 int qos_count, const int *granted_qos) {
  LOG("-- subscribed successfully\n");
}

/* Register the callbacks that the mosquitto connection will use. */
static bool set_callbacks(struct mosquitto *m) {
  mosquitto_connect_callback_set(m, on_connect);
  mosquitto_publish_callback_set(m, on_publish);
  mosquitto_subscribe_callback_set(m, on_subscribe);
  mosquitto_message_callback_set(m, on_message);
  return true;
}

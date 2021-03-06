
#include <sys/time.h>
#include <sys/types.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <termios.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <sys/file.h>
#include <string.h>
#include <math.h>

#include "zwave.h"
#include "genlist.h"

#define MAX_CMD_SZ	128
#define MAX_ZWAVE_NODES	256

typedef struct zwave_msg {
	list_node list;
	char	cmd[ MAX_CMD_SZ ];
	int	len;
	int	port;
	int	resp_req;
}zwave_msg_S;

typedef struct zwave_node {
	int				id;
	unsigned char mode;
	unsigned char func;
	unsigned char basic_type;
	unsigned char gen_type;
	unsigned char spec_type;
} zwave_node_S;

typedef struct zwave_node_list {
	zwave_node_S	node[ MAX_ZWAVE_NODES ];
	int		node_idx;
	int		node_info;
} zwave_node_list_S;

LIST_HEAD( msg_list );
LIST_HEAD( ack_wait_list );
LIST_HEAD( resp_wait_list );
pthread_mutex_t list_lock;
zwave_node_list_S node_list;

int open_port (const char Pname[],int *port)
{
        struct termios portterm;
        if ((*port = open(Pname, O_RDWR | O_NOCTTY)) < 0) return (ERR_OPEN);
 
        if (!isatty(*port)) {
                close(*port);
                return (ERR_OPEN);
        }
        if (flock(*port, LOCK_EX | LOCK_NB) < 0) {
                close(*port);
                return (ERR_FLOCK);
        }
        portterm.c_cflag = CS8 | CREAD | CLOCAL;

        portterm.c_cc[VMIN] = 1; 
        portterm.c_cc[VTIME] = 0;

        cfsetispeed(&portterm, BAUDRATE);
        cfsetospeed(&portterm, BAUDRATE);

        portterm.c_lflag = 0;

        portterm.c_iflag = IGNBRK;
        portterm.c_oflag = 0;


        tcflush(*port, TCIOFLUSH);
        if (tcsetattr(*port, TCSANOW, &portterm) < 0) {
                close(*port);
                return (ERR_STTY);
        }
        usleep (1000);  
	tcflush(*port, TCIOFLUSH);

        return (0);
}


int read_port (int dev,char pnt[],int len,long timeout)
{
        int bytes = 0;
        int total = 0;
        struct timeval tv;
        fd_set fs;

        while (total < len) {
                FD_ZERO (&fs);
                FD_SET (dev,&fs);
                tv.tv_sec = 0;
                tv.tv_usec = 100000;
                bytes = select (FD_SETSIZE,&fs,NULL,NULL,&tv);

                // 0 bytes or error
                if( bytes <= 0 )
                {
                        return total;
                }

                bytes = read (dev,pnt+total,len-total);
                total += bytes;
        }

        return total;
}

int write_port (int dev,char pnt[],int len)
{
        int res;

        res = write (dev,pnt,len);
        if (res != len) {
                        exit (-1);
                return (ERR_TIMEOUT);
        }
        tcflush(dev, TCIOFLUSH);

        return (0);
}

void print_line( unsigned char *buff, int len )
{
	int idx = 0;
	while( idx < len ) printf( "%02X ", buff[ idx++ ] );
}

char checksum(char *buf, int len) {
        int ret = 0xff;
	int i;
        for (i=0;i<len;i++) ret ^= buf[i];
        return ret;
}

int send_first_message( )
{
	zwave_msg_S *req = NULL;
	int rc = 0;

	if ( list_empty( &msg_list ) )return 0;

	req = (zwave_msg_S *)list_pop_front( &msg_list );
	if ( !req ) return 1;	

	rc = write_port( req->port, req->cmd, req->len );

	list_add((list_node *)&ack_wait_list, (list_node *)req);
	//free( req );
	return 0;
}

int send_request( int port, unsigned char *buff, int len, int resp_req )
{
	zwave_msg_S *req;
	int index = 0;
	int i;

	pthread_mutex_lock (&list_lock);
	req = calloc( 1, sizeof( zwave_msg_S ) );
	if ( !req ) {
		printf("calloc failed\n");
		return 1;
	}
	req->cmd[ index++ ] = SOF;
	req->cmd[ index++ ] = len + 2 ;
	req->cmd[ index++ ] = REQUEST ;

	for (i=0; i<len;i++ ) req->cmd[index++] = buff[i];

	req->cmd[ index ] = checksum(req->cmd+1,len+2 );
	req->port = port;
	req->len = len + 4;
	req->resp_req = resp_req;

	list_add((list_node *)&msg_list, (list_node *)req);
	pthread_mutex_unlock (&list_lock);
	return 0;
}

void purge_first_resp_wait_list()
{
	zwave_msg_S *req = NULL;
	if ( list_empty( &resp_wait_list ) ) {
		printf("FATAL: No msgs in resp wait Q\n");
		goto out;
	}

	req = (zwave_msg_S *)list_pop_front( &resp_wait_list );
	if ( !req ) { 
		printf("FATAL: Failed to pop msg from the resp wait Q\n");
		goto out;
	}
	free( req );
out:
	return;
}

void process_frame( int port, unsigned char *frame, int length ) 
{
	int k; // int helper
	char tempbuf[512];
	char tempbuf2[512];
	static int ournodeid = -1;
	if (frame[0] == RESPONSE) {
		switch ((unsigned char)frame[1]) {
			case FUNC_ID_ZW_GET_SUC_NODE_ID:
				printf("\nGot reply to GET_SUC_NODE_ID, node: %d",frame[2]);
				if ((unsigned char)frame[2] == 0) {
					printf( "No SUC, we become SUC");
					tempbuf[0]=FUNC_ID_ZW_ENABLE_SUC;
					tempbuf[1]=1; // 0=SUC,1=SIS
					// tempbuf[2]=ZW_SUC_FUNC_BASIC_SUC;
					tempbuf[2]=ZW_SUC_FUNC_NODEID_SERVER;
					send_request( port, tempbuf , 3, RESP_NOT_REQ ); 
					tempbuf[0]=FUNC_ID_ZW_SET_SUC_NODE_ID;
					tempbuf[1]=ournodeid;
					tempbuf[2]=1; // TRUE, we want to be SUC/SIS
					tempbuf[3]=0; // no low power
					tempbuf[4]=ZW_SUC_FUNC_NODEID_SERVER;
					send_request( port, tempbuf , 5, RESP_REQ  );
				} else if ((unsigned char)frame[2] != ournodeid) {
					printf( "requesting network update from SUC");
					tempbuf[0]=FUNC_ID_ZW_REQUEST_NETWORK_UPDATE;
					send_request( port, tempbuf, 1, RESP_REQ );
				}
				break;
			;;
			case ZW_GET_ROUTING_INFO:
				printf("\nGot reply to ZW_GET_ROUTING_INFO:");
				break;
			;;
			case ZW_MEMORY_GET_ID:
				printf("\nGot reply to ZW_MEMORY_GET_ID:");
				printf("\nHome id: 0x%02x%02x%02x%02x, our node id: %d",(unsigned char) frame[2],(unsigned char) frame[3],(unsigned char)frame[4],(unsigned char)frame[5],(unsigned char)frame[6]);
				ournodeid = (unsigned char)frame[6];
				break;
			;;
			case ZW_MEM_GET_BUFFER:
				printf("\nGot reply to ZW_MEM_GET_BUFFER");
				break;
			;;
			case ZW_MEM_PUT_BUFFER:
				printf("\nGot reply to ZW_MEM_PUT_BUFFER");
				break;
			;;
			case FUNC_ID_SERIAL_API_GET_INIT_DATA:
				printf("\nGot reply to FUNC_ID_SERIAL_API_GET_INIT_DATA:");
				if (frame[4] == MAGIC_LEN) {
					int i, j;
					for (i=5;i<5+MAGIC_LEN;i++) {
						for (j=0;j<8;j++) {
							// printf("test node %d\n",(i-5)*8+j+1);
							if ((unsigned char)frame[i] & (0x01 << j)) {
								// printf("\nfound node %d",(i-5)*8+j+1);
								// requesting node protocol information
								// tempbuf[0]=0x60; // req. node info frame - test
								//addIntent((i-5)*8+j+1,1);
								tempbuf[0]=FUNC_ID_ZW_GET_NODE_PROTOCOL_INFO;
								tempbuf[1]=(i-5)*8+j+1;
								printf("\n Requesting protocol info for: %d", (i-5)*8+j+1);
								node_list.node[node_list.node_info++].id = (i-5)*8+j+1;
								send_request( port, tempbuf , 2, RESP_NOT_REQ );
							} else {
								printf("\nNode id %i not in node mask, deleting",(i-5)*8+j+1); 
							}
						}
					}
				}
				break;
			;;
			case FUNC_ID_ZW_GET_NODE_PROTOCOL_INFO:
				printf("\nGot reply to FUNC_ID_ZW_GET_NODE_PROTOCOL_INFO:");
				node_list.node[ node_list.node_idx ].mode = frame[ 2 ];
				node_list.node[ node_list.node_idx ].func = frame[ 3 ];
				node_list.node[ node_list.node_idx ].basic_type = frame[ 5 ];
				node_list.node[ node_list.node_idx ].gen_type = frame[ 6 ];
				node_list.node[ node_list.node_idx++ ].spec_type = frame[ 7 ];

				// test if node is valid
				if (frame[6] != 0) {
					int tmp_nodeid;
					printf("\n***FOUND NODE: %d",tmp_nodeid);

					if (((unsigned char)frame[2]) & (0x01 << 7)) {
						printf( "listening node");
						// request version from the device
						//zwRequestVersion(tmp_nodeid);
						//zwRequestManufacturerSpecificReport(tmp_nodeid);
					} else {
						printf( "sleeping node");
					}
					if (((unsigned char)frame[3]) & (0x01 << 7)) {
						printf( "optional functionality");
					}
					if (((unsigned char)frame[3]) & (0x01 << 6)) {
						printf( "1000ms frequent listening slave");
					}
					if (((unsigned char)frame[3]) & (0x01 << 5)) {
						printf( "250ms frequent listening slave");
					}
					switch (frame[5]) {
						case BASIC_TYPE_CONTROLLER:
							printf( "BASIC TYPE: Controller");
							break;
						;;
						case BASIC_TYPE_STATIC_CONTROLLER:
							printf( "BASIC TYPE: Static Controller");
							break;
						;;
						case BASIC_TYPE_SLAVE:
							printf( "BASIC TYPE: Slave");
							break;
						;;
						case BASIC_TYPE_ROUTING_SLAVE:
							printf( "BASIC TYPE: Routing Slave");
							break;
						;;
						default:
							printf( "BASIC TYPE: %x",frame[5]);
							break;
						;;
					}
					switch ((unsigned char)frame[6]) {
						case GENERIC_TYPE_GENERIC_CONTROLLER:
							printf( "GENERIC TYPE: Generic Controller");
							break;
						;;
						case GENERIC_TYPE_STATIC_CONTROLLER:
							printf( "GENERIC TYPE: Static Controller");
							break;
						;;
						case GENERIC_TYPE_THERMOSTAT:
							printf( "GENERIC TYPE: Thermostat");
							break;
						;;
						case GENERIC_TYPE_SWITCH_MULTILEVEL:
							printf( "GENERIC TYPE: Multilevel Switch");
							break;
						;;
						case GENERIC_TYPE_SWITCH_REMOTE:
							printf( "GENERIC TYPE: Remote Switch");
							// TODO: saner approach
							//  we read the associations from groups one and two (left and right paddle on the ACT remote switch)
							//zwAssociationGet(tmp_nodeid,1);
							//zwAssociationGet(tmp_nodeid,2);
							break;
						;;
						case GENERIC_TYPE_SWITCH_BINARY:
							printf( "GENERIC TYPE: Binary Switch");
							break;
						;;
						case GENERIC_TYPE_SENSOR_BINARY:
							printf( "GENERIC TYPE: Sensor Binary");
							break;
						case GENERIC_TYPE_WINDOW_COVERING:
							printf( "GENERIC TYPE: Window Covering");
							break;
						;;
						case GENERIC_TYPE_SENSOR_MULTILEVEL:
							printf( "GENERIC TYPE: Sensor Multilevel");
							break;
						;;
						case GENERIC_TYPE_SENSOR_ALARM:
							printf( "GENERIC TYPE: Sensor Alarm");
							break;
						;;
						default:
							printf( "GENERIC TYPE: 0x%x",frame[6]);
							break;
						;;

					}
					printf( "SPECIFIC TYPE: 0x%x",frame[7]);

				} else {
					printf("Invalid generic class (0x%x), ignoring device",(unsigned char)frame[6]);
				}
				break;
			;;
			case FUNC_ID_ZW_REQUEST_NODE_INFO:
				printf( "\nGot reply to FUNC_ID_ZW_REQUEST_NODE_INFO:");
				break;
			;;
			case FUNC_ID_ZW_SEND_DATA:
				switch(frame[2]) {
					case 1:
						printf("\nZW_SEND delivered to Z-Wave stack");
						purge_first_resp_wait_list();
						break;
					case 0:
						printf( "\nERROR: ZW_SEND could not be delivered to Z-Wave stack");
						break;
					default:
						printf( "\nERROR: ZW_SEND Response is invalid!");
				}

				break;
			case FUNC_ID_SERIAL_API_GET_CAPABILITIES:
				printf( "\nGot reply to FUNC_ID_SERIAL_API_GET_CAPABILITIES:");
				printf("\nSerAppV:%i,r%i,Manf %i,Typ %i,Prod %i",(unsigned char)frame[2],(unsigned char)frame[3], ((unsigned char)frame[4]<<8) + (unsigned char)frame[5],((unsigned char)frame[6]<<8) + (unsigned char)frame[7],((unsigned char)frame[8]<<8) + (unsigned char)frame[9]);	
				break;
			case ZW_GET_VERSION:
				printf( "\nGot reply to ZW_VERSION:");
				printf("\nZWave Version: %c.%c%c",(unsigned char)frame[9],(unsigned char)frame[11],(unsigned char)frame[12]);	
				break;
			default:
				printf( "\nTODO: handle response for 0x%x ",(unsigned char)frame[1]);
				break;
			;;
		}

	} else if (frame[0] == REQUEST) {

		switch (frame[1]) {
			case FUNC_ID_ZW_SEND_DATA:
			{
				printf("\nZW_SEND Response with callback %i received",(unsigned char)frame[2]);
			}
			break;
			case FUNC_ID_ZW_ADD_NODE_TO_NETWORK:
				printf( "\nFUNC_ID_ZW_ADD_NODE_TO_NETWORK:");
				switch (frame[3]) {
					case ADD_NODE_STATUS_LEARN_READY:
						printf( "ADD_NODE_STATUS_LEARN_READY");
						break;
					case ADD_NODE_STATUS_NODE_FOUND:
						printf( "ADD_NODE_STATUS_NODE_FOUND");
						break;
					case ADD_NODE_STATUS_ADDING_SLAVE:
						printf( "ADD_NODE_STATUS_ADDING_SLAVE");
						printf( "Adding node id %i",(unsigned int)frame[4]);
						if (((unsigned int)frame[7] == 8) && ((unsigned int)frame[8] == 3)) {
							printf( "Setback schedule thermostat detected, triggering configuration");
//							zwWakeupSet((unsigned char)frame[4],15,true);
						}
						// finish adding node	
//						zwAddNodeToNetwork(0,false);
						break;
					case ADD_NODE_STATUS_ADDING_CONTROLLER:
						printf( "ADD_NODE_STATUS_ADDING_CONTROLLER");
						printf( "Adding node id %i",(unsigned int)frame[4]);
						break;
					case ADD_NODE_STATUS_PROTOCOL_DONE:
						printf( "ADD_NODE_STATUS_PROTOCOL_DONE");
						// we send no replication info for now
//						zwAddNodeToNetwork(0,false);
						break;
					case ADD_NODE_STATUS_DONE:
						printf( "ADD_NODE_STATUS_DONE");
						break;
					case ADD_NODE_STATUS_FAILED:
						printf( "ADD_NODE_STATUS_FAILED");
						break;
					default:
						break;
				}
				break;
			case FUNC_ID_ZW_REMOVE_NODE_FROM_NETWORK:
				printf( "\nFUNC_ID_ZW_REMOVE_NODE_FROM_NETWORK:");
				switch (frame[3]) {
					case REMOVE_NODE_STATUS_LEARN_READY:
						printf( "REMOVE_NODE_STATUS_LEARN_READY");
						break;
					case REMOVE_NODE_STATUS_NODE_FOUND:
						printf( "REMOVE_NODE_STATUS_NODE_FOUND");
						break;
					case REMOVE_NODE_STATUS_ADDING_SLAVE:
						printf( "REMOVE_NODE_STATUS_ADDING_SLAVE");
						// printf( "ing node id %i",(unsigned int)frame[4]);
						// finish adding node	
//						zwRemoveNodeFromNetwork(0);
						break;
					case REMOVE_NODE_STATUS_ADDING_CONTROLLER:
						printf( "REMOVE_NODE_STATUS_ADDING_CONTROLLER");
						// printf( "ing node id %i",(unsigned int)frame[4]);
						break;
					case REMOVE_NODE_STATUS_PROTOCOL_DONE:
						printf( "REMOVE_NODE_STATUS_PROTOCOL_DONE");
//						zwRemoveNodeFromNetwork(0);
						break;
					case REMOVE_NODE_STATUS_DONE:
						printf( "REMOVE_NODE_STATUS_DONE");
						break;
					case REMOVE_NODE_STATUS_FAILED:
						printf( "REMOVE_NODE_STATUS_FAILED");
						break;
					default:
						break;
				}
				break;

			case FUNC_ID_APPLICATION_COMMAND_HANDLER:
				printf( "\nFUNC_ID_APPLICATION_COMMAND_HANDLER:");
				switch ((unsigned char)frame[5]) {
					case COMMAND_CLASS_CONTROLLER_REPLICATION:
						printf( "COMMAND_CLASS_CONTROLLER_REPLICATION");
						// 0x1 0xb 0x0 0x4 0x2 0xef 0x5 0x21 0x31 0x7 0x1 0x1 0xf (#######!1####)
						if (frame[6] == CTRL_REPLICATION_TRANSFER_GROUP) {
							// we simply ack the group information for now
							tempbuf[0]=FUNC_ID_ZW_REPLICATION_COMMAND_COMPLETE;
							send_request( port, tempbuf , 1, RESP_NOT_REQ );
						} else {
							// ack everything else, too
							tempbuf[0]=FUNC_ID_ZW_REPLICATION_COMMAND_COMPLETE;
							send_request( port, tempbuf , 1, RESP_NOT_REQ );
						}
						break;
					;;
					case COMMAND_CLASS_MULTI_INSTANCE:
						printf( "COMMAND_CLASS_MULTI_INSTANCE");
						if (frame[6] == MULTI_INSTANCE_REPORT) {
						        int instanceCount = (unsigned char)frame[8];
							printf( "Got MULTI_INSTANCE_REPORT from node %i: Command Class 0x%x, instance count: %i",(unsigned char)frame[3],(unsigned char)frame[7], instanceCount);
							// instance count == 1 -> assume instance 1 is "main" device and don't add new device
						} else if (frame[6] == MULTI_INSTANCE_CMD_ENCAP) {
						        printf( "Got MULTI_INSTANCE_CMD_ENCAP from node %i: instance %i Command Class 0x%x type 0x%x",(unsigned char)frame[3],(unsigned char)frame[7],(unsigned char)frame[8],(unsigned char)frame[9]);
						        if (frame[8] == COMMAND_CLASS_SENSOR_MULTILEVEL) {
/*								
							        handleCommandSensorMultilevelReport(frame[3], //nodeid
												    (unsigned int)frame[7], // instance id
												    (unsigned int)frame[10], // sensor type
												    (unsigned int)frame[11], // value metadata
												    (unsigned int)frame[12], // value
												    (unsigned int)frame[13],
												    (unsigned int)frame[14],
												    (unsigned int)frame[15]);
*/
								printf( "CommandSensorMultilevelReport: node=%i, inst=%i, type=%i, valuem=%i, value=%i", 
										frame[3], frame[7], frame[10], frame[11], frame[12] );
						        } else if ((frame[8] == COMMAND_CLASS_BASIC) && (frame[9] == BASIC_REPORT)) {
								// 41	07/22/10 12:05:17.485		0x1 0xc 0x0 0x4 0x0 0x7 0x6 0x60 0x6 0x1 0x20 0x3 0x0 0xb2 (#######`## ###) <0xb795fb90>
								// 36	07/22/10 12:05:17.485		FUNC_ID_APPLICATION_COMMAND_HANDLER: <0xb795fb90>
								// 36	07/22/10 12:05:17.485		COMMAND_CLASS_MULTI_INSTANCE <0xb795fb90>
								// 36	07/22/10 12:05:17.485		Got MULTI_INSTANCE_CMD_ENCAP from node 7: instance 1 Command Class 0x20 type 0x3 <0xb795fb90>

								printf( "Got basic report from node %i, instance %i, value: %i",(unsigned char)frame[3],(unsigned char)frame[7], (unsigned char) frame[10]);
//								DCEcallback->SendLightChangedEvents ((unsigned char)frame[3], (unsigned char)frame[7], (unsigned char) frame[10]);

						        } else if ((frame[8] == COMMAND_CLASS_BASIC) && (frame[9] == BASIC_SET)) {
								printf( "Got basic set from node %i, instance %i, value: %i",(unsigned char)frame[3],(unsigned char)frame[7], (unsigned char) frame[10]);
//								DCEcallback->SendOnOffEvent ((unsigned char)frame[3],(unsigned char)frame[7],(unsigned char) frame[10]);
							}
						}
						break;
					;;	
					case COMMAND_CLASS_VERSION:
						printf( "COMMAND_CLASS_VERSION");
						if (frame[6] == VERSION_REPORT) {
							printf( "REPORT: Lib.typ: 0x%x, Prot.Ver: 0x%x, Sub: 0x%x App.Ver: 0x%x, Sub: 0x%x",
								(unsigned char)frame[7], (unsigned char)frame[8], (unsigned char)frame[9], (unsigned char)frame[10], (unsigned char)frame[11]);
						}

						break;
					;;
					case COMMAND_CLASS_METER:
						printf( "COMMAND_CLASS_METER");
						if (frame[6] == METER_REPORT) {
							printf( "Got meter report from node %i",(unsigned char)frame[3]);
							int scale = ( (unsigned char)frame[8] & METER_REPORT_SCALE_MASK ) >> METER_REPORT_SCALE_SHIFT;
							int precision = ( (unsigned char)frame[8] & METER_REPORT_PRECISION_MASK ) >> METER_REPORT_PRECISION_SHIFT;
							int size = (unsigned char)frame[8] & METER_REPORT_SIZE_MASK;
							int value;
							short tmpval;
							switch(size) {
								case 1:
									value = (signed char)frame[9];
									;;
								break;
								case 2:
									tmpval = ((unsigned char)frame[9] << 8) + (unsigned char)frame[10];
									value = (signed short)tmpval;
									;;
								break;
								default:
									value = ( (unsigned char)frame[9] << 24 ) + ( (unsigned char)frame[10] << 16 ) + ( (unsigned char)frame[11] << 8 ) + (unsigned char)frame[12];
									value = (signed int)value;
									;;
								break;
							}
							printf( "METER DEBUG: precision: %i scale: %i size: %i value: %i",precision,scale,size,value);
							if (precision > 0) { value = value / pow(10 , precision) ; }  // we only take the integer part for now
							switch(((unsigned char)frame[7]) & 0x1f) { // meter type
								case METER_REPORT_ELECTRIC_METER:
									switch (scale) {
										case 0:
											printf( "Electric meter measurement received: %d kWh",value);
											break;
										case 1:
											printf( "Electric meter measurement received: %d kVAh",value);
											break;
										case 2:
											printf( "Electric meter measurement received: %d W",value);
//											DCEcallback->SendPowerUsageChangedEvent((unsigned char)frame[3], -1, value);
											break;
										case 3:
											printf( "Electric meter measurement received: %d pulses",value);
											break;
									} 
									break;
								case METER_REPORT_GAS_METER:
									switch (scale) {
										case 0:
											printf( "Gas meter measurement received: %d m2",value);
											break;
										case 1:
											printf( "Gas meter measurement received: %d cubic feet",value);
											break;
										case 3:
											printf( "Gas meter measurement received: %d pulses",value);
											break;
									} 
									break;
								case METER_REPORT_WATER_METER:
									switch (scale) {
										case 0:
											printf( "Water meter measurement received: %d m2",value);
											break;
										case 1:
											printf( "Water meter measurement received: %d cubic feet",value);
											break;
										case 2:
											printf( "Water meter measurement received: %d US gallons",value);
											break;
										case 3:
											printf( "Water meter measurement received: %d pulses",value);
											break;
									} 
									break;
								default:
									printf( "unknown METER_REPORT received: %i",(unsigned char)frame[7]);
									break;
							}
						}
						break;
					;;
					case COMMAND_CLASS_MANUFACTURER_SPECIFIC:
						printf( "COMMAND_CLASS_MANUFACTURER_SPECIFIC");
						if (frame[6] == MANUFACTURER_SPECIFIC_REPORT) {
							int manuf=0;int prod=0; int type=0;

							manuf = ((unsigned char)frame[7]<<8) + (unsigned char)frame[8];
							type = ((unsigned char)frame[9]<<8) + (unsigned char)frame[10];
							prod = ((unsigned char)frame[11]<<8) + (unsigned char)frame[12];

							printf( "REPORT: Manuf: 0x%x, Prod Typ: 0x%x, Prod 0x%x", manuf,type,prod);
//							parseManufacturerSpecific((unsigned char)frame[3],manuf,type,prod);

							/* switch ( manuf ) {
								case 0x64: printf("Popp\n");
								break;;
								default: printf("none\n");
							}	*/ 
							
						}

						break;
					;;
					case COMMAND_CLASS_WAKE_UP:
						printf( "COMMAND_CLASS_WAKE_UP - ");
						// 0x1 0x8 0x0 0x4 0x4 0x2 0x2 0x84 0x7 0x74 (#########t)
						// if ((COMMAND_CLASS_WAKE_UP == frame[5]) && ( frame[6] == WAKE_UP_NOTIFICATION)) {

						if (frame[6] == WAKE_UP_NOTIFICATION) {
/*					
							// we got a wake up frame, make sure we remember the device does not always listen
							ZWNodeMapIt = ZWNodeMap.find((unsigned int)frame[3]);
							if (ZWNodeMapIt != ZWNodeMap.end()) {
								(*ZWNodeMapIt).second->sleepingDevice=true;
								if ((*ZWNodeMapIt).second->typeGeneric == GENERIC_TYPE_SENSOR_MULTILEVEL) {
								        // use multi instance get for value > 0, else use normal sensor report
								        if ((*ZWNodeMapIt).second->mapCCInstanceCount.find(COMMAND_CLASS_SENSOR_MULTILEVEL) != (*ZWNodeMapIt).second->mapCCInstanceCount.end() &&
									    (*ZWNodeMapIt).second->mapCCInstanceCount[COMMAND_CLASS_SENSOR_MULTILEVEL] > 0) {
									  printf( "Requesting SENSOR_REPORT for instances 1 to %d, node %d",(*ZWNodeMapIt).second->mapCCInstanceCount[COMMAND_CLASS_SENSOR_MULTILEVEL],frame[3]);
									        for (int i = 1; i <= (*ZWNodeMapIt).second->mapCCInstanceCount[COMMAND_CLASS_SENSOR_MULTILEVEL]; i++) {
										        zwRequestMultilevelSensorReportInstance((unsigned int)frame[3], i);
										}
									} else {
									        zwRequestMultilevelSensorReport((unsigned int)frame[3]);
									}
								}
							}

							// read battery level
							zwGetBatteryLevel((unsigned char) frame[3]);

							// inject commands from the sleeping queue for this nodeid
							wakeupHandler((unsigned char) frame[3]);	
*/
							// handle broadcasts from unconfigured devices
							if (frame[2] & RECEIVE_STATUS_TYPE_BROAD ) { 
								printf( "Got broadcast wakeup from node %i, doing WAKE_UP_INTERVAL_SET",frame[3]);
/*								if (ournodeid != -1) { 
									zwWakeupSet((unsigned char)frame[3],60,false); // wakeup interval
									wakeupHandler((unsigned char) frame[3]); // fire commands, device is awake
								}
*/							} else {
								printf( "Got unicast wakeup from node %i, doing WAKE_UP_NO_MORE_INFORMATION",frame[3]);
								// send to sleep
								tempbuf[0]=FUNC_ID_ZW_SEND_DATA;
								tempbuf[1]=frame[3]; // destination
								tempbuf[2]=2; // length
								tempbuf[3]=COMMAND_CLASS_WAKE_UP;
								tempbuf[4]=WAKE_UP_NO_MORE_INFORMATION;
								tempbuf[5]=TRANSMIT_OPTION_ACK | TRANSMIT_OPTION_AUTO_ROUTE;
								send_request( port, tempbuf, 6, RESP_NOT_REQ );
								// ...
							}//
						}
						break;
					case COMMAND_CLASS_SENSOR_BINARY:
						printf( "\nCOMMAND_CLASS_SENSOR_BINARY - ");
						if (frame[6] == SENSOR_BINARY_REPORT) {
							printf( "Got sensor report from node %i, level: %i",(unsigned char)frame[3],(unsigned char)frame[7]);
						}
						break;
					;;
					case COMMAND_CLASS_SENSOR_MULTILEVEL:
						printf( "\nCOMMAND_CLASS_SENSOR_MULTILEVEL - ");
						if (frame[6] == SENSOR_MULTILEVEL_REPORT) {
/*						        handleCommandSensorMultilevelReport((unsigned int)frame[3], // node id
											    -1, // instance id
											    (unsigned int)frame[7], // sensor type
											    (unsigned int)frame[8], // value metadata
											    (unsigned int)frame[9], // value
											    (unsigned int)frame[10],
											    (unsigned int)frame[11],
											    (unsigned int)frame[12]);
*/							printf( "CommandSensorMultilevelReport: node=%i, inst=%i, type=%i, valuem=%i, value=%i", 
									frame[3], frame[7], frame[10], frame[11], frame[12] );
						}
						break;
					;;
					case COMMAND_CLASS_SENSOR_ALARM:
						printf( "\nCOMMAND_CLASS_SENSOR_ALARM - ");
						if ((unsigned char)frame[6] == SENSOR_ALARM_REPORT) {
							printf( "Got sensor report from node %i, source node ID: %i, sensor type: %i, sensor state: %i",(unsigned char)frame[3],(unsigned char)frame[7],(unsigned char)frame[8],(unsigned char)frame[9]);
							if ((unsigned char)frame[9]!=0) { // ALARM!!!
								/* if ((unsigned char)frame[0]==0xff) {

								} else { // percentage of severity

								} 
								switch ((unsigned char)frame[8]) {
									case 0x0: // general purpose
										break;
									case 0x1: // smoke alarm
										DCEcallback->SendFireAlarmEvent (frame[3], -1);
										break;
									case 0x2: // co alarm
										DCEcallback->SendAirQualityEvent (frame[3], -1);
										break;
									case 0x3: // co2 alarm
										DCEcallback->SendAirQualityEvent (frame[3], -1);
										break;
									case 0x4: // heat alarm
										DCEcallback->SendFireAlarmEvent (frame[3], -1);
										break;
									case 0x5: // water leak alarm
										break;
								}*/
							}
							
						}

					case COMMAND_CLASS_SWITCH_MULTILEVEL:
						printf( "\nCOMMAND_CLASS_SWITCH_MULTILEVEL - ");
						if (frame[6] == SWITCH_MULTILEVEL_REPORT) {
							printf( "Got switch multilevel report from node %i, level: %i",(unsigned char)frame[3],(unsigned char)frame[7]);
							printf( "Send light changed event");
						} else if ((unsigned char)frame[6] == SWITCH_MULTILEVEL_SET) {
							printf( "Got switch multilevel set from node %i, level: %i",(unsigned char)frame[3],(unsigned char)frame[7]);
							printf( "Send light changed event");
						}
						break;
					case COMMAND_CLASS_SWITCH_ALL:
						printf( "\nCOMMAND_CLASS_SWITCH_ALL - ");
						if (frame[6] == SWITCH_ALL_ON) {
							printf( "Got switch all ON from node %i",(unsigned char)frame[3]);
							printf( "Send light changed event");
						}
						if (frame[6] == SWITCH_ALL_OFF) {
							printf( "Got switch all OFF from node %i",(unsigned char)frame[3]);
							printf( "Send light changed event");
						}
						break;
					case COMMAND_CLASS_ALARM:
						printf( "\nCOMMAND_CLASS_ALARM - ");
						if (frame[6] == ALARM_REPORT) {
							printf( "Got ALARM from node %i, type: %i, level: %i",(unsigned char)frame[3],(unsigned char)frame[7],(unsigned char)frame[8]);
							printf( "Send sensor tripped changed event");
						}
						break;
					case COMMAND_CLASS_BASIC:
						printf( "COMMAND_CLASS_BASIC - ");
						if (frame[6] == BASIC_REPORT) {

							// char tmp[32];
							// sprintf(&tmp,"%d",frame[7]);
							//
							printf( "Got basic report from node %i, value: %i",(unsigned char)frame[3],(unsigned char)frame[7]);
						} else if ((unsigned char)frame[6] == BASIC_SET) {
							printf( "Got BASIC_SET from node %i, value %i",(unsigned char)frame[3],(unsigned char)frame[7]);
							
						} else {
							printf( "Got COMMAND_CLASS_BASIC: %i, ignoring",(unsigned char)frame[6]);
									
							// zwSendBasicReport((unsigned char)frame[3]);
						}						
						break;
					case COMMAND_CLASS_CLIMATE_CONTROL_SCHEDULE:
						printf( "COMMAND_CLASS_CLIMATE_CONTROL_SCHEDULE: - ");
						if (frame[6] == SCHEDULE_GET) {
							printf( "Got SCHEDULE_GET from node %i for day: %i",frame[3],frame[7]);
						}
						break;
					case COMMAND_CLASS_ASSOCIATION:
						printf( "COMMAND_CLASS_ASSOCIATION - ");
						break;
					case COMMAND_CLASS_BATTERY:
						if ((unsigned char)frame[6] == BATTERY_REPORT) {
							printf( "COMMAND_CLASS_BATTERY:BATTERY_REPORT: Battery level: %d",(unsigned char)frame[7]);
	
							if ((unsigned char)frame[7] == 0xff) {
								printf( "Battery low warning from node %d",(unsigned char)frame[3]);
							}
						}
						break;
						;;
					case COMMAND_CLASS_THERMOSTAT_SETPOINT:
						if ((unsigned char)frame[6] == THERMOSTAT_SETPOINT_GET) {
							printf( "COMMAND_CLASS_THERMOSTAT_SETPOINT:THERMOSTAT_SETPOINT_GET received from node %d",(unsigned char)frame[3]);
						}
						if ((unsigned char)frame[6] == THERMOSTAT_SETPOINT_REPORT) {
							printf( "COMMAND_CLASS_THERMOSTAT_SETPOINT:THERMOSTAT_SETPOINT_REPORT received from node %d",(unsigned char)frame[3]);
						}
					case COMMAND_CLASS_SWITCH_BINARY:
						if ((unsigned char)frame[6] == SWITCH_BINARY_SET) {
							printf( "COMMAND_CLASS_SWITCH_BINARY:SWITCH_BINARY_SET received from node %d value %d",(unsigned char)frame[3],(unsigned char)frame[7]);
						}
						if ((unsigned char)frame[6] == SWITCH_BINARY_GET) {
							printf( "COMMAND_CLASS_SWITCH_BINARY:SWITCH_BINARY_GET received from node %d value %d",(unsigned char)frame[3],(unsigned char)frame[7]);
						}
						if ((unsigned char)frame[6] == SWITCH_BINARY_REPORT) {
							printf( "COMMAND_CLASS_SWITCH_BINARY:SWITCH_BINARY_REPORT received from node %d value %d",(unsigned char)frame[3],(unsigned char)frame[7]);
						}
						
					case COMMAND_CLASS_CLOCK:
						if ((unsigned char)frame[6] == CLOCK_GET) {
							printf( "COMMAND_CLASS_CLOCK:CLOCK_GET received from node %d",(unsigned char)frame[3]);
//							zwSetClock((unsigned char)frame[3]);
						} else if ((unsigned char)frame[6] == CLOCK_REPORT) {
							printf( "COMMAND_CLASS_CLOCK:CLOCK_REPORT received from node %d",(unsigned char)frame[3]);
						} else if ((unsigned char)frame[6] == CLOCK_SET) {
							printf( "COMMAND_CLASS_CLOCK:CLOCK_SET received from node %d",(unsigned char)frame[3]);
						}
						break;
						;;
					case COMMAND_CLASS_THERMOSTAT_MODE:
						printf( "COMMAND_CLASS_THERMOSTAT_MODE - ");
						if (((unsigned char)frame[6] == THERMOSTAT_MODE_REPORT)||((unsigned char)frame[6] == THERMOSTAT_MODE_SET)) {
							switch((unsigned char)frame[7]) {
								case 0:
									printf( "Thermostat node %d Mode Off",(unsigned char)frame[3]);
									break;
								case 1:
									printf( "Thermostat node %d Mode Heat",(unsigned char)frame[3]);
									break;
								case 2:
									printf( "Thermostat node %d Mode Cool",(unsigned char)frame[3]);
									break;
								case 3:
									printf( "Thermostat node %d Mode Auto",(unsigned char)frame[3]);
									break;
								case 4:
									printf( "Thermostat node %d Mode Aux/Emergency Heat",(unsigned char)frame[3]);
									break;
								case 5:
									printf( "Thermostat node %d Mode Resume",(unsigned char)frame[3]);
									break;
								case 6:
									printf( "Thermostat node %d Mode Fan Only",(unsigned char)frame[3]);
									break;
								case 7:
									printf( "Thermostat node %d Mode Furnace",(unsigned char)frame[3]);
									break;
								case 8:
									printf( "Thermostat node %d Mode Dry Air",(unsigned char)frame[3]);
									break;
								case 9:
									printf( "Thermostat node %d Mode Moist Air",(unsigned char)frame[3]);
									break;
								case 10:
									printf( "Thermostat node %d Mode Auto Changeover",(unsigned char)frame[3]);
									break;

							}
						}
						break;
						;;
					case COMMAND_CLASS_MULTI_CMD:
						printf( "COMMAND_CLASS_MULTI_CMD - ");
						break;
					default:
						printf( "Function not implemented - unhandled command class: %x",(unsigned char)frame[5]);
						break;
					;;
				}
				break;
			;;
			case FUNC_ID_ZW_APPLICATION_UPDATE:
				switch((unsigned char)frame[2]) {
					case UPDATE_STATE_NODE_INFO_RECEIVED:
						printf( "FUNC_ID_ZW_APPLICATION_UPDATE:UPDATE_STATE_NODE_INFO_RECEIVED received from node %d - ",(unsigned int)frame[3]);
						switch((unsigned char)frame[5]) {
							case BASIC_TYPE_ROUTING_SLAVE:
							case BASIC_TYPE_SLAVE:
								//printf( "BASIC_TYPE_SLAVE:");
								switch(frame[6]) {
									case GENERIC_TYPE_SWITCH_MULTILEVEL:
										printf( "GENERIC_TYPE_SWITCH_MULTILEVE");
										tempbuf[0] = FUNC_ID_ZW_SEND_DATA;
										tempbuf[1] = frame[3];
										tempbuf[2] = 0x02;
										tempbuf[3] = COMMAND_CLASS_SWITCH_MULTILEVEL;
										tempbuf[4] = SWITCH_MULTILEVEL_GET;
										tempbuf[5] = TRANSMIT_OPTION_ACK | TRANSMIT_OPTION_AUTO_ROUTE;
										send_request ( port, tempbuf,6, RESP_REQ );
										tempbuf[0] = FUNC_ID_ZW_SEND_DATA;
										tempbuf[1] = frame[3];
										tempbuf[2] = 0x02;
										tempbuf[3] = COMMAND_CLASS_BASIC;
										tempbuf[4] = BASIC_GET;
										tempbuf[5] = TRANSMIT_OPTION_ACK | TRANSMIT_OPTION_AUTO_ROUTE;
										send_request( port, tempbuf,6, RESP_NOT_REQ );
										break;
									case GENERIC_TYPE_SWITCH_BINARY:
										printf( "GENERIC_TYPE_SWITCH_BINARY");
										tempbuf[0] = FUNC_ID_ZW_SEND_DATA;
										tempbuf[1] = frame[3];
										tempbuf[2] = 0x02;
										tempbuf[3] = COMMAND_CLASS_BASIC;
										tempbuf[4] = BASIC_GET;
										tempbuf[5] = TRANSMIT_OPTION_ACK | TRANSMIT_OPTION_AUTO_ROUTE;
										send_request( port, tempbuf,6, RESP_NOT_REQ );
										break;
									case GENERIC_TYPE_SWITCH_REMOTE:
										printf( "GENERIC_TYPE_SWITCH_REMOTE");
										break;
									case GENERIC_TYPE_SENSOR_MULTILEVEL:
										printf( "GENERIC_TYPE_SENSOR_MULTILEVEL");
										break;
									;;
									default:
										printf( "unhandled class");
										break;
									;;
								}
							default:
								break;
							;;
						}
						break;
					case UPDATE_STATE_NODE_INFO_REQ_FAILED:
						printf( "FUNC_ID_ZW_APPLICATION_UPDATE:UPDATE_STATE_NODE_INFO_REQ_FAILED received");
						break;
				        case UPDATE_STATE_NEW_ID_ASSIGNED:
					{
						printf( "** Network change **: ID %d was assigned to a new Z-Wave node",(unsigned char)frame[3]);
					}
						break;
					case UPDATE_STATE_DELETE_DONE:
						printf( "** Network change **: Z-Wave node %d was removed",(unsigned char)frame[3]);
						break;
					;;
					default:
						break;
					;;
				}	
				break;
			;;
			default:
			;;


		}

	} else {
		// should not happen
	}
	return;
}

int wait_list_empty()
{
	return ( list_empty( &ack_wait_list ) && list_empty( &resp_wait_list ) );
}

void *start( void *p )
{
        int port = *(int *)p;
        int rc = 0;
        struct timeval tv;
        fd_set fs;
        unsigned char buffer[256];
	int flen = 0;
	zwave_msg_S *req = NULL;

	while( 1 ) {
		memset( buffer, 0, 256 );
		rc = read_port( port, buffer, 1, 100 );
		if ( 1 != rc ) { 
			if ( wait_list_empty() ) {
				rc = send_first_message( );
				if ( rc ) {
					printf( "\nsending message failed\n" );
				}
			}
			continue;
		}

		switch( buffer[ 0 ] ) {
		case ACK:
			printf( "\nACK received\n" );
			if ( list_empty( &ack_wait_list ) ) {
				printf("FATAL: No msgs in ack wait Q\n");
				break;
			}

			req = (zwave_msg_S *)list_pop_front( &ack_wait_list );
			if ( !req ) { 
				printf("FATAL: Failed to pop msg from the ack wait Q\n");
				break;
			}
			if ( req->resp_req ) {
				list_add((list_node *)&resp_wait_list, (list_node *)req);
				break;	
			}

			free( req );
			break;
		case NAK:
			printf( "\nNAK received\n" );
			break;
		case CAN:
			printf( "\nCAN received\n" );
			break;
		case SOF:
			printf( "\nSOF received\n" );
			rc = read_port( port, buffer + 1, 1, 100 );
			if ( 1 != rc ) {
				printf( "read framelen failed/timedout %d", rc );
				break;
			}
			flen = buffer[ 1 ];
			printf(" Framelen: %d\n", flen);
			rc = read_port( port, buffer + 2, flen, 100 );
			if ( flen != rc ) {
				printf( "read frame failed/timedout %d", rc );
				break;
			}
			print_line( buffer, flen );
			buffer[0] = ACK; 
			write_port( port, buffer, 1 );
			process_frame( port, buffer + 2, flen - 2 );

			break;
		default:
			printf( "\nreceived: %02x", buffer[ 0 ] );
		}
	}
}

void print_nodes()
{
	int idx = 0;
	for( idx = 0; idx < node_list.node_idx; idx++ ) {
		zwave_node_S *node = &node_list.node[ idx ];
		printf( "Node %d-%i: ", idx, node->id );
		if( node->gen_type ) {
			if (node->mode && (0x01 << 7)) {
				printf("listening node, ");
			} else {
				printf("sleeping node, ");
			}
			if (node->func && (0x01 << 7)) {
				printf("optional functionality, ");
			}
			switch ( node->basic_type ) {
				case BASIC_TYPE_CONTROLLER:
					printf("BASIC TYPE: Controller, ");
					break;
				;;
				case BASIC_TYPE_STATIC_CONTROLLER:
					printf("BASIC TYPE: Static Controller, ");
					break;
				;;
				case BASIC_TYPE_SLAVE:
					printf("BASIC TYPE: Slave, ");
					break;
				;;
				case BASIC_TYPE_ROUTING_SLAVE:
					printf("BASIC TYPE: Routing Slave, ");
					break;
				;;
				default:
					printf("BASIC TYPE: %x, ", node->basic_type);
					break;
				;;
			}
			switch ( node->gen_type ) {
				case GENERIC_TYPE_GENERIC_CONTROLLER:
					printf("GENERIC TYPE: Generic Controller, ");
					break;
				;;
				case GENERIC_TYPE_STATIC_CONTROLLER:
					printf("GENERIC TYPE: Static Controller, ");
					break;
				;;
				case GENERIC_TYPE_THERMOSTAT:
					printf("GENERIC TYPE: Thermostat, ");
					break;
				;;
				case GENERIC_TYPE_SWITCH_MULTILEVEL:
					printf("GENERIC TYPE: Multilevel Switch, ");
					break;
				;;
				case GENERIC_TYPE_SWITCH_REMOTE:
					printf("GENERIC TYPE: Remote Switch, ");
					break;
				;;
				case GENERIC_TYPE_SWITCH_BINARY:
					printf("GENERIC TYPE: Binary Switch, ");
					break;
				;;
				case GENERIC_TYPE_SENSOR_BINARY:
					printf("GENERIC TYPE: Sensor Binary, ");
					break;
				case GENERIC_TYPE_WINDOW_COVERING:
					printf("GENERIC TYPE: Window Covering, ");
					break;
				;;
				default:
					printf("GENERIC TYPE: %x, ", node->gen_type);
					break;
				;;

			}
			printf("SPECIFIC TYPE: %d\n", node->spec_type);

		} else {
			printf("Invalid generic class (%i), ignoring device\n", node->gen_type);
		}
	}

}

int send_binary_sw_get( int port ) {
        char buff[1024];
	int node_id = 3;

	buff[0] = FUNC_ID_ZW_SEND_DATA;
	buff[1] = node_id;
	buff[2] = 2;
	buff[3] = COMMAND_CLASS_SWITCH_BINARY;
	buff[4] = SWITCH_BINARY_GET;
	buff[5] = TRANSMIT_OPTION_ACK | TRANSMIT_OPTION_AUTO_ROUTE;
	buff[6] = 3;
	return send_request( port, buff, 7, RESP_REQ );
}

int send_binary_sw_set( int port, int level ) {
        char buff[1024];
	int node_id = 3;
	buff[0] = FUNC_ID_ZW_SEND_DATA;
	buff[1] = node_id;
	buff[2] = 3;
	buff[3] = COMMAND_CLASS_SWITCH_BINARY;
	buff[4] = SWITCH_BINARY_SET;
	buff[5] = level;
	buff[6] = TRANSMIT_OPTION_ACK | TRANSMIT_OPTION_AUTO_ROUTE;
	return send_request( port, buff, 7, RESP_REQ );
}

int main()
{
	int port = 0;
	int rc = 0;
	unsigned char buffer[256];
	pthread_t readThread;
	int idx = 0;
	int opt = 0;

	rc = open_port ( "/dev/ttyUSB0", &port );	
	if ( rc )
		printf("Failed to open port\n");

	memset( &node_list, 0, sizeof( zwave_node_list_S ) );
	pthread_mutex_init(&list_lock, NULL);

        buffer[0] = 0x15; //NAK
        write( port, buffer, 1 );
        pthread_create(&readThread, NULL, start, (void*)&port);

        buffer[0] = ZW_GET_VERSION;
        send_request( port, buffer , 1, RESP_NOT_REQ );

        buffer[0] = ZW_MEMORY_GET_ID;
        send_request( port, buffer , 1, RESP_NOT_REQ );

        buffer[0] = FUNC_ID_SERIAL_API_GET_CAPABILITIES;
        send_request( port, buffer , 1, RESP_NOT_REQ );

        buffer[0] = FUNC_ID_ZW_GET_SUC_NODE_ID;
        send_request( port, buffer , 1, RESP_NOT_REQ );

        buffer[0] = FUNC_ID_SERIAL_API_GET_INIT_DATA;
        send_request( port, buffer , 1, RESP_NOT_REQ );

	do {

		printf("\n=== MENU ===\n");
		printf("1. List Nodes\n");
		printf("2. Switch state ON\n");
		printf("3. Switch state OFF\n");
		printf("4. SWitch State Get\n");
		printf("5. Exit\n");
		
		scanf( "%d", &opt );
		switch( opt ) {
		case 1:
			print_nodes();
			break;
		case 2:
			send_binary_sw_set( port, 0 );
			break;
		case 3:
			send_binary_sw_set( port, 255 );
			break;
		case 4:
			send_binary_sw_get( port );
			break;
		case 5:
			break;
		default:
			printf("Not Implemented\n");
			break;
		}

	} while(opt != 5); 


        close( port );
        return 0;
}
	


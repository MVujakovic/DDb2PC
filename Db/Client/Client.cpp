#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib,"User32.lib")

#define _CRTDBG_MAP_ALLOC  
#include <stdlib.h>  
#include <crtdbg.h>  

#include "Communication.h"
#include "ClusterLifecycle.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <time.h>
#include <strsafe.h>

#include <atomic>
#include <map>

#define MASTER_PORT_FOR_CLIENTS 27019
#define CLIENT_CONNECT_TIMEOUT 15000 // 15 seconds
std::map<int, ExtendedEndpointElement> nodesEndpoints;
CRITICAL_SECTION cs_NodeEndp;

SOCKET connectToMasterSocket = INVALID_SOCKET;
SOCKET connectToNodeSocket = INVALID_SOCKET;

int myId = -1;
Message clientRegistrationMsg;

Errors trOnLineError = TRANSACTION_ONLINE;
Errors unknownError = UNKNOWN;

int SendRegMsgToMaster();
int GetClusterDataFromMaster(Message* registrationReply);
void PrintChooseNodeMenu();
void PrintNodeCommunicationMenu(int nodeId);
int ConnectWithNode(int nodeId);
void CommunicateWithNode(int nodeId);
void ProcessReceivedDataMessage(Message* dataMessage);

int __cdecl main(int argc, char **argv)
{
	//_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

	int iResult;
	DWORD currentProcId;
	currentProcId = GetCurrentProcessId();
	printf("\n*** PROCES ID= %d ****\n", currentProcId);

	if (argc != 2)
	{
		printf("usage: %s server-name\n", argv[0]);
		return 1;
	}

	if (InitializeWindowsSockets() == false)
	{
		return 1;
	}

	InitializeCriticalSection(&cs_NodeEndp);

	bool isRegistered = false, isUserCanceled = false;
	Message registrationReply;
	registrationReply.size = 0;

	do
	{
		if (connectToTarget(&connectToMasterSocket, argv[1], MASTER_PORT_FOR_CLIENTS) != 0)
		{
			// expeceted behaviour in case of unsuccessfull connecting is exiting...
			char answer = 'x';
			printf("\nPress y/Y for keep trying, or any other key for exit. ");
			scanf("%c", &answer);
			if (answer == 'y' || answer == 'Y')
			{
				Sleep(1000);
				continue;
			}
			break;
		}

		printf("\nMy Id: ");
		scanf("%d", &myId);

		char title[80];
		sprintf_s(title, 80, "Client Id=%d", myId);
		SetConsoleTitleA(title);

		if (SendRegMsgToMaster() == 1)
		{
			printf("\nSomething went wrong with registration to Master.");
			break;
		}

		if ((iResult = receiveMessage(connectToMasterSocket, &registrationReply, 50, 3, 20, true)) != 0)
		{
			printf("\nSomething went wrong with receiving cluster information.");
			switch (iResult)
			{
				case ENETDOWN:
				{
					printf("\n WSAENETDOWN");
				}
				break;
				case WSAECONNABORTED:
				{
					printf("\n WSAECONNABORTED");
				}
				break;
				case WSAECONNRESET:
				{
					printf("\n WSAECONNRESET");
				}
				break;

				case TIMED_OUT:
				{
					printf("\n TIMED_OUT");
				}
				break;

				case CLOSED_GRACEFULLY:
				{
					printf("\n CLOSED_GRACEFULLY");
				}
				break;
				default:
				{
					printf("\n OTHER ERROR");
				}
				break;
			}
			break;
		}
		switch (registrationReply.msgType)
		{
			case Registration:
			{
				isRegistered = true;
				printf("\nClient is registered to master.");

				if (registrationReply.size != sizeof(MsgType)) // if any node exists in cluster
				{
					int  sizeOfClusterInfoData = registrationReply.size - sizeof(MsgType);
					int countOfClusterNodes = sizeOfClusterInfoData / sizeof(ExtendedEndpointElement);

					printf("\n Number of possible nodes to connect with = %d", countOfClusterNodes);

					for (int i = 0; i < countOfClusterNodes; i++)
					{				
						ExtendedEndpointElement* ptrEe = (ExtendedEndpointElement*)(registrationReply.payload + i * sizeof(ExtendedEndpointElement));
						ExtendedEndpointElement eeToStore;
						eeToStore.ipAddress = ptrEe->ipAddress;
						eeToStore.port = ptrEe->port;
						eeToStore.endpointId = ptrEe->endpointId;
						StoreEndpoint(&nodesEndpoints, &cs_NodeEndp, eeToStore.endpointId, &eeToStore);
					}
				}
			}
			break;
			case Error:
			{
				// expeceted behaviour in case of non uniqe id is termination
				char answer = 'x';
				Errors rcvdError = (Errors)*(int*)registrationReply.payload;
				if (rcvdError == NON_UNIQUE_ID)
				{
					printf("\nERROR: Id not unique. ");
					printf("\nPress y/Y for keep trying, or any other key for exit.");
					while ((getchar()) != '\n');
					scanf("%c", &answer);
				}
				if (closesocket(connectToMasterSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("processing reg reply==error -> closesocket"));
				}

				// if user decided to try with other id number
				if (answer == 'y' || answer == 'Y')
				{
					continue;
				}
				else
					isUserCanceled = true;
			}
			break;
			default:
			{
				printf("\nerror: invalid RegistratioReply type");
				isUserCanceled = true;
			}
			break;
		}

		if (registrationReply.size > 4)
			free(registrationReply.payload);

	} while (!(isRegistered || isUserCanceled));


	if (isRegistered)
	{
		int selectedNodeId = -1;
		if (nodesEndpoints.size() == 0)
			printf("\nThere is currently no available nodes to connect with. Try again later.");
		else
		{
			PrintChooseNodeMenu();
			scanf("%d", &selectedNodeId);

			if (ConnectWithNode(selectedNodeId) == 0)
			{
				CommunicateWithNode(selectedNodeId);
				char title[80];
				sprintf_s(title, 80, "CLIENT Id=%d --- NODE Id=%d", myId, selectedNodeId);
				SetConsoleTitleA(title);
			}
		}
	}

	printf("\nPress any key to exit...");
	getchar();

	DeleteCriticalSection(&cs_NodeEndp);
	if (closesocket(connectToMasterSocket) == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("closesocket(connectToMasterSocket)"));
	}
	WSACleanup();

	return 0;
}

int SendRegMsgToMaster()
{
	int iResult = -1;
	clientRegistrationMsg.msgType = Registration;
	clientRegistrationMsg.size += sizeof(MsgType);
	clientRegistrationMsg.size += 4;
	clientRegistrationMsg.payload = (char*)&myId;

	if ((iResult = sendMessage(connectToMasterSocket, &clientRegistrationMsg, 100, 0, INFINITE_ATTEMPT_NO, true)) != clientRegistrationMsg.size + 4)
	{
		printf("\nSomething went wrong with sending registration message");
		switch (iResult)
		{
			case ENETDOWN:
			{
				printf("\n WSAENETDOWN");
			}
			break;
			case WSAECONNABORTED:
			{
				printf("\n WSAECONNABORTED");
			}
			break;
			case WSAECONNRESET:
			{
				printf("\n WSAECONNRESET");
			}
			break;
			case TIMED_OUT:
			{
				printf("\n TIMED_OUT");
			}
			break;
			case CLOSED_GRACEFULLY:
			{
				printf("\n CLOSED_GRACEFULLY");
			}
			break;
			default:
			{
				// maybe WSAENOBUFS - 2000 max
				printf("\n OTHER ERROR");
			}
			break;
		}
		return 1;
	}
	return 0;
}

int GetClusterDataFromMaster(Message* registrationReply)
{
	int iResult;
	printf("\nClient is trying to get cluster information from master...");
	if ((iResult = receiveMessage(connectToMasterSocket, registrationReply, 200, 0, 200, true)) != 0)
	{
		printf("\nSomething went wrong with receiving cluster information.");
		switch (iResult)
		{
			case ENETDOWN:
			{
				printf("\n WSAENETDOWN");
			}
			break;
			case WSAECONNABORTED:
			{
				printf("\n WSAECONNABORTED");
			}
			break;
			case WSAECONNRESET:
			{
				printf("\n WSAECONNRESET");
			}
			break;

			case TIMED_OUT:
			{
				printf("\n TIMED_OUT");
			}
			break;

			case CLOSED_GRACEFULLY:
			{
				printf("\n CLOSED_GRACEFULLY");
			}
			break;

			default:
			{
				printf("\n OTHER ERROR");
			}
			break;
		}
		return 1;
	}
	return 0;
}

void PrintChooseNodeMenu()
{
	system("cls");
	int ch;
	printf("\n=================== DDb Nodes =================");
	printf("\n\n Choose a node to connect with...");
	printf("\n");

	for (auto it = nodesEndpoints.begin(); it != nodesEndpoints.end(); ++it)
	{
		printf("\n Node < id=%d > ", it->first);
	}

	printf("\n option -> \n	 ");

	while ((ch = getchar()) != '\n' && ch != EOF);
}

void PrintNodeCommunicationMenu(int nodeId)
{
	printf("\n================ Communication with <node %d>==============", nodeId);
	printf("\n\n Choose an operation");
	printf("\n");
	printf("\n 1. Write message");
	printf("\n 2. Read all messages");
	//printf("\n 2. Read my messages");
	//printf("\n 3. Update my message");
	//printf("\n 3. Delete my message");
	printf("\n 4. Exit...");
	printf("\n option -> \n	 ");
	fflush(stdout);
	//while ((getchar()) != '\n');
}

int ConnectWithNode(int nodeId)
{
	int iResult;

	std::map<int, ExtendedEndpointElement>::iterator targetIt = nodesEndpoints.find(nodeId);
	if (targetIt == nodesEndpoints.end()) // if does not exist
		return 1;
	else
	{
		ExtendedEndpointElement targetNode = targetIt->second;
		connectToNodeSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (connectToNodeSocket == INVALID_SOCKET)
		{
			ErrorHandlerTxt(TEXT("ConnectWithNode"));
			return 1;
		}

		sockaddr_in serverAddress;
		serverAddress.sin_family = AF_INET;
		serverAddress.sin_addr.s_addr = targetNode.ipAddress;
		serverAddress.sin_port = htons(targetNode.port);

		if (SetSocketToNonBlocking(&connectToNodeSocket) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("ConnectWithNode.SetSocketToNonBlocking to non-blocking"));
			return 1;
		}

		if (connect(connectToNodeSocket, (SOCKADDR*)&serverAddress, sizeof(serverAddress)) == SOCKET_ERROR)
		{
			int errCode = WSAGetLastError();
			if (errCode != WSAEWOULDBLOCK)
			{
				ErrorHandlerTxt(TEXT("ConnectWithNode.connect -> errCode != WSAEWOULDBLOCK"));
				if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("ConnectWithNode.connect.closesocket"));
				}
				return 1;
			}
			else
			{
				FD_SET writeSet, errSet;
				FD_ZERO(&writeSet);
				FD_SET(connectToNodeSocket, &writeSet);
				FD_ZERO(&errSet);
				FD_SET(connectToNodeSocket, &errSet);
				timeval timeVal;
				timeVal.tv_sec = 0;
				timeVal.tv_usec = CLIENT_CONNECT_TIMEOUT;

				iResult = select(0, NULL, &writeSet, &errSet, &timeVal);
				if (iResult == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("ConnectWithNode.connect.select"));
					if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("ConnectWithNode.connect.select.closesocket"));
					}
					return 1;
				}
				else if (iResult == 0)
				{
					printf("\n Time limit expired for node select");
					if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("ConnectWithNode.connect.select.closesocket - time expired"));
					}
					return 1;
				}
				else
				{
					if (FD_ISSET(connectToNodeSocket, &errSet))
					{
						DWORD errCode = 0;
						int len = sizeof(errCode);
						if (getsockopt(connectToNodeSocket, SOL_SOCKET, SO_ERROR, (char*)&errCode, &len) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("getsockopt"));
						}
						else
						{
							printf("\n error: %d ", errCode);
							if (errCode == WSAECONNREFUSED)
							{
								printf("\nConnection to node id = %d refused. Possibly trying to connect to a service that is inactive.", targetNode.endpointId);
							}
						}
						if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("ConnectWithNode.select > 0 -> getsockopt.closesocket"));
						}
						return 1;
					}
				}
			}
		}
	}
	return 0;
}

void CommunicateWithNode(int nodeId)
{
	int iResult;
	bool isCanceled = false;
	int option;
	do {
		PrintNodeCommunicationMenu(nodeId);
		scanf("%d", &option);
		while ((getchar()) != '\n'); // flushes the standard input -> (clears the input buffer)

		switch (option)
		{
			case 1:
			{
				// write 
				char helpBuffer[DEFAULT_BUFLEN];

				ClientMessageHeader payloadHeader;
				payloadHeader.clientId = myId;
				payloadHeader.originCounter = -1;
				payloadHeader.originId = -1;
				payloadHeader.reqType = WRITE;
				int sizeOfPayloadHeader = 16;

				memcpy(helpBuffer, &payloadHeader, sizeOfPayloadHeader);
				char* concretePayload = helpBuffer + sizeOfPayloadHeader;

				printf("\nEnter message:\n");
				fgets(concretePayload, DEFAULT_BUFLEN - sizeOfPayloadHeader, stdin);
				int tempLength = strlen(concretePayload);
				concretePayload[tempLength] = 0;

				int payloadSize = sizeOfPayloadHeader + strlen(concretePayload); // saljemo poruku sa \n na kraju i u strlen ulazi i \n

				Message msgToSend;
				msgToSend.size = 0;
				msgToSend.msgType = Data;
				msgToSend.size += sizeof(MsgType); // sizeof previous field 

				msgToSend.payload = helpBuffer;
				msgToSend.size += payloadSize;

				if ((iResult = sendMessage(connectToNodeSocket, &msgToSend, 50, 3, 10, false)) != msgToSend.size + 4)
				{
					printf("\n Sending WRITE request message to node failed.");
					switch (iResult)
					{
						case ENETDOWN:
						{
							printf("\n WSAENETDOWN");
							isCanceled = true;
						}
						break;
						case WSAECONNABORTED:
						{
							printf("\n WSAECONNABORTED");
						}
						break;
						case WSAECONNRESET:
						{
							printf("\n WSAECONNRESET");
						}
						break;
						case TIMED_OUT:
						{
							printf("\n TIMED_OUT");
						}
						break;
						case CLOSED_GRACEFULLY:
						{
							printf("\n CLOSED_GRACEFULLY");
						}
						break;
						default:
						{
							printf("\n OTHER ERROR");
						}
						break;
					}

					if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("Communication with node.sendMessage-> closesocket"));
					}
				}
				else
				{
					printf("\nWRITE request message sent sucessfully...");

					Message rcvdDataNodeReply;
					rcvdDataNodeReply.size = 0;
					if ((iResult = receiveMessage(connectToNodeSocket, &rcvdDataNodeReply, 200, 10, 10, true)) == 0)
					{
						printf("\n Reply to WRITE request received from node...");
						if (rcvdDataNodeReply.msgType == Error)
						{
							if ((int)rcvdDataNodeReply.payload == trOnLineError)
							{
								printf("\n error: Transaction is already On Line");
							}
							else if ((int)rcvdDataNodeReply.payload == unknownError)
							{
								printf("\n error: unknown");
							}
						}
					}
					else
					{
						printf("\n Receiving reply to WRITE request failed...");

					}

					if (rcvdDataNodeReply.size > 4)
						free(rcvdDataNodeReply.payload);

				}

			}
			break;
			case 2:
			{
				// read all 
				ClientMessageHeader payloadHeader;
				payloadHeader.clientId = myId;
				payloadHeader.originCounter = -1;
				payloadHeader.originId = -1;
				payloadHeader.reqType = READ;
				int sizeOfPayloadHeader = 16;

				Message msgToSend;
				msgToSend.size = 0;
				msgToSend.msgType = Data;
				msgToSend.size += sizeof(MsgType); // sizeof previous field 

				msgToSend.payload = (char*)&payloadHeader;
				msgToSend.size += sizeOfPayloadHeader;

				if ((iResult = sendMessage(connectToNodeSocket, &msgToSend, 50, 3, 10, false)) != msgToSend.size + 4)
				{
					printf("\n Sending READ request message to node failed.");
					switch (iResult)
					{
						case ENETDOWN:
						{
							printf("\n WSAENETDOWN");
							isCanceled = true;
						}
						break;
						case WSAECONNABORTED:
						{
							printf("\n WSAECONNABORTED");
						}
						break;
						case WSAECONNRESET:
						{
							printf("\n WSAECONNRESET");
						}
						break;
						case TIMED_OUT:
						{
							printf("\n TIMED_OUT");
						}
						break;
						case CLOSED_GRACEFULLY:
						{
							printf("\n CLOSED_GRACEFULLY");
						}
						break;
						default:
						{
							printf("\n OTHER ERROR");
						}
						break;
					}

					if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("Communication with node.sendMessage-> closesocket"));
					}
				}
				else
				{
					printf("\nREAD request message sent sucessfully...");

					Message rcvdNodeReply;
					rcvdNodeReply.size = 0;
					if ((iResult = receiveMessage(connectToNodeSocket, &rcvdNodeReply, 200, 10, 10, true)) == 0)
					{
						printf("\n Reply to READ request received from node...");
						
						if (rcvdNodeReply.msgType == Error)
						{
							if ((int)rcvdNodeReply.payload == trOnLineError)
							{
								printf("\n error: Transaction is already On Line");
							}
							else if ((int)rcvdNodeReply.payload == unknownError)
							{
								printf("\n error: unknown");
							}
						}
						else
						{
							ProcessReceivedDataMessage(&rcvdNodeReply);
						}					
					}
					else
					{
						printf("\n Receiving reply to READ request failed...");

					}

					if (rcvdNodeReply.size > 4)
						free(rcvdNodeReply.payload);

				}		
			}
			break;
			case 3:
				printf("Not supported yet...");
			{
				/*
				// delete my messages
				char helpBuffer[DEFAULT_BUFLEN];

				ClientMessageHeader payloadHeader;
				payloadHeader.clientId = myId;
				payloadHeader.originCounter = -1;
				payloadHeader.originId = -1;
				payloadHeader.reqType = REMOVE_ALL;
				int sizeOfPayloadHeader = 16;

				memcpy(helpBuffer, &payloadHeader, sizeOfPayloadHeader);			
				int payloadSize = sizeOfPayloadHeader; 

				Message msgToSend;
				msgToSend.size = 0;
				msgToSend.msgType = Data;
				msgToSend.size += sizeof(MsgType); // sizeof previous field 

				msgToSend.payload = helpBuffer;
				msgToSend.size += payloadSize;

				if ((iResult = sendMessage(connectToNodeSocket, &msgToSend, 50, 3, 10, false)) != msgToSend.size + 4)
				{
					printf("\n Sending REMOVE_ALL request message to node failed.");
					switch (iResult)
					{
						case ENETDOWN:
						{
							printf("\n WSAENETDOWN");
							isCanceled = true;
						}
						break;
						case WSAECONNABORTED:
						{
							printf("\n WSAECONNABORTED");
						}
						break;
						case WSAECONNRESET:
						{
							printf("\n WSAECONNRESET");
						}
						break;
						case TIMED_OUT:
						{
							printf("\n TIMED_OUT");
						}
						break;
						case CLOSED_GRACEFULLY:
						{
							printf("\n CLOSED_GRACEFULLY");
						}
						break;
						default:
						{
							printf("\n OTHER ERROR");
						}
						break;
					}

					if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("Communication with node.sendMessage-> closesocket"));
					}
				}
				else
				{
					printf("\nWRITE request message sent sucessfully...");

					Message rcvdDataNodeReply;
					rcvdDataNodeReply.size = 0;
					if ((iResult = receiveMessage(connectToNodeSocket, &rcvdDataNodeReply, 200, 10, 10, true)) == 0)
					{
						printf("\n Reply to REMOVE_ALL request received from node...");

						if (rcvdDataNodeReply.msgType == Error)
						{
							if ((int)rcvdDataNodeReply.payload == trOnLineError)
							{
								printf("\n error: Transaction is already On Line");
							}
							else if ((int)rcvdDataNodeReply.payload == unknownError)
							{
								printf("\n error: unknown");
							}
						}
					}
					else
					{
						printf("\n Receiving reply to REMOVE_ALL request failed...");

					}

					if (rcvdDataNodeReply.size > 4)
						free(rcvdDataNodeReply.payload);

				}
				*/
			}
			break;
			case 4:
			{
				// exit
			}
			break;
			default:
			{
				printf("Wrong Option. Enter again\n");
			}
			break;
		}
	} while (option != 4);
}

void ProcessReceivedDataMessage(Message* dataMessage)
{
	int payloadSize = dataMessage->size - 4;
	Message msg;
	int idx = 0;
	int currentlyRead = 0;
	while (true)
	{
		if (currentlyRead == payloadSize)
			break;

		msg = *(Message*)((dataMessage->payload) + idx);
		char* help = (dataMessage->payload) + idx;
		msg.payload = help;

		size_t nonConcretePayloadDataSize = 4 + sizeof(MsgType) + sizeof(ClientMessageHeader);
		int wholeMessageSize = msg.size+ 4;
		
		size_t printSize = wholeMessageSize - sizeof(ClientMessageHeader) - sizeof(MsgType) - 4;
		printf("\n ");
		printf(" -> %.*s", printSize, msg.payload + nonConcretePayloadDataSize);

		idx += 4;
		idx += msg.size;
		currentlyRead = idx;	
	}
}
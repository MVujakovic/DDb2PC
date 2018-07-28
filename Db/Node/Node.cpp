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
#include "NodeStorage.h"

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

#define MASTER_PORT_FOR_NODES 27017 

Message nodeRegistrationMsg; // registration Message as a payload have NodeRegData
NodeRegData MyEndpointRegData;

// list of alive database nodes (peers)
std::map<int, SOCKET> nodeSockets;
CRITICAL_SECTION cs_NodeSockets;

std::map<int, HANDLE> nodeCommunicationHandles;
CRITICAL_SECTION cs_NodeCommHandles;

std::map<int, SOCKET> clientSockets;
CRITICAL_SECTION cs_ClientSockets;

void InitializeCriticalSections();
void DeleteCriticalSections();

int PrepareSocketEndpoint(SOCKET* socket, USHORT* port, const char* endpointIdDescription);
int JoinCluster(NodeRegData* registrationData, Message* registrationReply);
void ConnectWithPeers(char * peersInfo, int peersCount);
void DoIntegrityUpdate();
bool SendRequestForTransactionCoordinatorRole();

DWORD WINAPI ConnectWithOnePeer(LPVOID lpParam);
DWORD WINAPI masterCommunicationListener(LPVOID lpParam);
DWORD WINAPI listenForNodes(LPVOID lpParam);
DWORD WINAPI listenForClients(LPVOID lpParam);

DWORD WINAPI clientCommunication(LPVOID lpParam);
DWORD WINAPI dispatchTransactionRequest(LPVOID lpParam);
DWORD WINAPI sendTransactionRequestToNodeAndGetReply(LPVOID lpParam);
DWORD WINAPI getTransactionRequestAndSendReply(LPVOID lpParam);

Errors trOnLineError = TRANSACTION_ONLINE;
Errors unknownError = UNKNOWN;

HANDLE TransactionCoordinatorRequest_Signal;
HANDLE TransactionPhase_StartSignal;
HANDLE TransactionPhase_FinishSignal;
std::atomic<bool> TransactionPhaseResult;

std::atomic<bool> exitSignal(false);
std::atomic<bool> isIntegrityUpToDate(false);
std::atomic<bool> isTransactionOnLine(false);
std::atomic<bool> isTransactionCoordinator(false);

SOCKET connectToMasterSocket = INVALID_SOCKET;
SOCKET listenForNodesSocket = INVALID_SOCKET;
SOCKET listenForClientsSocket = INVALID_SOCKET;

StorageManager storage;

Message requestFromClient;
Message transactionRequestForNode;

int __cdecl main(int argc, char **argv)
{
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

	HANDLE nodesListener_Handle = NULL, masterListener_Handle = NULL, clientsListener_Handle = NULL;
	DWORD masterListener_Id, nodesListener_Id, clientsListener_Id;

	DWORD currentProcId;
	currentProcId = GetCurrentProcessId();
	printf("\n*** PROCES ID= %d ****\n", currentProcId);

	if (argc != 3)
	{
		// node kao 2 argument unosi SVOJU ADRESU
		printf("usage: %s server-name\n", argv[0]);
		return 1;
	}

	if (InitializeWindowsSockets() == false)
	{
		return 1;
	}

	TransactionCoordinatorRequest_Signal = CreateSemaphore(0, 0, 1, NULL);
	TransactionPhase_StartSignal = CreateSemaphore(0, 0, 1, NULL);
	TransactionPhase_FinishSignal = CreateSemaphore(0, 0, 1, NULL);

	InitializeCriticalSections();

	InitStorage(&storage);

	long ip = inet_addr(argv[2]);
	MyEndpointRegData.intIpAddress = ip; // little endian value of ipaddress
	if (PrepareSocketEndpoint(&listenForNodesSocket, &MyEndpointRegData.portForNodes, "SOCKET FOR SERVING NODES") ||
		PrepareSocketEndpoint(&listenForClientsSocket, &MyEndpointRegData.portForClients, "SOCKET FOR SERVING CLIENTS") != 0)
	{
		printf("\nPreparing endpoints for serving nodes/clients failed.");
	}
	else
	{
		bool isRegistered = false, isUserCanceled = false;
		do
		{
			if (connectToTarget(&connectToMasterSocket, argv[1], MASTER_PORT_FOR_NODES) != 0)
			{
				exitSignal = true;
				break;
			}
			//printf("\nNode connected to master...");
			printf("\nMy Id: ");
			scanf("%d", &MyEndpointRegData.nodeId);

			char title[80];
			sprintf_s(title, 80, "NODE Id=%d, NodesPort: %u, ClientsPort: %u", MyEndpointRegData.nodeId, MyEndpointRegData.portForNodes, MyEndpointRegData.portForClients);
			SetConsoleTitleA(title);

			Message masterRegistrationReply;
			masterRegistrationReply.size = 0;
			if (JoinCluster(&MyEndpointRegData, &masterRegistrationReply) == 1)
			{
				printf("\nSomething went wrong with registration (JoinCluster).");
				if (masterRegistrationReply.size > 4)
					free(masterRegistrationReply.payload);

				exitSignal = true;
				break;
			}

			switch (masterRegistrationReply.msgType)
			{
				case Registration:
				{
					isRegistered = true;
					printf("\nNode is registered to master.");
					if (masterRegistrationReply.size != sizeof(MsgType)) // if it is not first node...
					{
						int  sizeOfPeersData = masterRegistrationReply.size - sizeof(MsgType);
						int countOfPeersData = sizeOfPeersData / sizeof(EndpointElement);
						//printf("\n Number of peers to connect with = %d", countOfPeersData);

						if (countOfPeersData != 0)
						{
							ConnectWithPeers(masterRegistrationReply.payload, countOfPeersData);
							printf("\n Successfully connected with peers");
						}
					}
				}
				break;

				case Error:
				{
					// expeceted behaviour in case of non uniqe id is termination
					char answer = 'x';
					printf("\nNode MasterRegistrationReply type == Error");
					Errors rcvdError = (Errors)*(int*)masterRegistrationReply.payload;

					if (rcvdError == NON_UNIQUE_ID)
					{
						printf("\nId not unique. ");
						printf("\nPress y/Y for keep trying, or any other key for exit.");
						while ((getchar()) != '\n');
						scanf("%c", &answer);
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

				case ShutDown:
				{
					printf("\n:D :( :D NE OVO RegistratioReply type == SHUTDOWN");
					exitSignal = true;
				}
				break;

				default:
				{
					printf("\nNI OVO!!! Node MasterRegistrationReply type is unsupported in current context.");
					exitSignal = true;
					isUserCanceled = true;
				}
				break;
			}

			if (masterRegistrationReply.size > 4)
				free(masterRegistrationReply.payload);

		} while (!(isRegistered || isUserCanceled || exitSignal));

		if (isRegistered)
		{
			nodesListener_Handle = CreateThread(NULL, 0, &listenForNodes, &MyEndpointRegData.nodeId, 0, &nodesListener_Id);
			if (nodesListener_Handle == NULL)
			{
				ErrorHandlerTxt(TEXT("CreateThread listenForNodes"));
			}
			else
			{
				masterListener_Handle = CreateThread(NULL, 0, &masterCommunicationListener, &MyEndpointRegData.nodeId, 0, &masterListener_Id);
				if (masterListener_Handle == NULL)
				{
					ErrorHandlerTxt(TEXT("CreateThread masterCommunication"));
				}
				else
				{
					clientsListener_Handle = CreateThread(NULL, 0, &listenForClients, &MyEndpointRegData.nodeId, 0, &clientsListener_Id);
					if (masterListener_Handle == NULL)
					{
						ErrorHandlerTxt(TEXT("CreateThread listenForClients"));
					}
				}
			}
		}
	}

	HANDLE eventHandles[] =
	{
		// order of handles is important in our case... 
		nodesListener_Handle,
		masterListener_Handle,
		clientsListener_Handle
	};

	int awaitNo = 0;
	for (int i = 0; i < sizeof(eventHandles) / sizeof(eventHandles[0]); i++)
	{
		if (eventHandles[i] != NULL)
		{
			awaitNo++;
			//printf("\n wait handles = %d", awaitNo);
		}
	}

	WaitForMultipleObjects(awaitNo, &eventHandles[0], true, INFINITE);

	if (connectToMasterSocket != INVALID_SOCKET)
	{
		if (closesocket(connectToMasterSocket) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("main.closesocket(connectToMasterSocket)"));
		}
	}

	DeleteCriticalSections();
	SAFE_DELETE_HANDLE(TransactionPhase_StartSignal);
	SAFE_DELETE_HANDLE(TransactionPhase_FinishSignal);
	SAFE_DELETE_HANDLE(nodesListener_Handle)
		SAFE_DELETE_HANDLE(masterListener_Handle);
	SAFE_DELETE_HANDLE(clientsListener_Handle);

	WSACleanup();

	printf("\nPress any key to exit...");
	scanf_s("%d");
	getc(stdin);

	printf("\nExiting...");
	Sleep(3000);
	return 0;
}

#pragma region Threads

#pragma region Listener Threads

DWORD WINAPI listenForClients(LPVOID lpParam)
{
	int iResult;
	int connectionCounter = 0;
	SOCKET acceptedClientSocket = INVALID_SOCKET;

	// socket is already binded in preparing endpoint function
	iResult = listen(listenForClientsSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("listenForClients.listen"));
		if (closesocket(listenForClientsSocket) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForClients.listen.closesocket(listenForClientsSocket)"));
		}
		exitSignal = true;
		return 1;
	}

	HANDLE transactionCoordinator = CreateThread(NULL, 0, dispatchTransactionRequest, NULL, 0, NULL);
	if (transactionCoordinator == NULL)
	{
		ErrorHandlerTxt(TEXT("CreateThread transactionRequestCoordination"));
		exitSignal = true;
		return 1;
	}

	printf("\n\n-----NODE: Clients Server initialized\n");

	while (true)
	{
		SOCKADDR_IN connectedClientAddr;
		int addrlen = sizeof(connectedClientAddr);
		FD_SET readSet;
		FD_ZERO(&readSet);
		FD_SET(listenForClientsSocket, &readSet);
		timeval timeVal;
		timeVal.tv_sec = 1;
		timeVal.tv_usec = 0;

		iResult = select(listenForClientsSocket, &readSet, NULL, NULL, &timeVal);
		if (iResult == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForClient.select"));
			break;
		}

		if (exitSignal)
			break;

		else if (FD_ISSET(listenForClientsSocket, &readSet))
		{
			acceptedClientSocket = accept(listenForClientsSocket, (SOCKADDR*)&connectedClientAddr, &addrlen);
			connectionCounter++;
			//printf("\n\n Accepted Clients - connection counter = %d", connectionCounter);
			if (acceptedClientSocket == INVALID_SOCKET)
			{
				iResult = WSAGetLastError();
				ErrorHandlerTxt(TEXT("listenForClients.accept"));

				if (iResult == WSAECONNRESET || iResult == WSAECONNABORTED)
					continue;

				break;
			}

			if (SetSocketToNonBlocking(&acceptedClientSocket) == SOCKET_ERROR)
			{
				if (closesocket(acceptedClientSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForclients.SetSocketToNonBlocking-> closesocket (acceptedClientSocket)"));
				}
				continue;
			}

			HANDLE clientCommHandler;
			clientCommHandler = CreateThread(NULL, 0, &clientCommunication, (LPVOID)acceptedClientSocket, 0, NULL);
			if (clientCommHandler == NULL)
			{
				ErrorHandlerTxt(TEXT("CreateThread clientCommunication failed"));
				if (shutdown(acceptedClientSocket, SD_SEND) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForClients >  CreateThread clientCommunication.shutdown"));
				}
				if (closesocket(acceptedClientSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForClients >  CreateThread clientCommunication.closesocket"));
				}
			}
			else
			{
				// maybe we do not have to store socket because each message contains info of origin ?
				//StoreHandle(&nodeCommunicationHandles, &cs_NodeCommHandles, acceptedPeerNodeId, &clientCommHandler);
			}
		}
	}

	exitSignal = true;

	if (closesocket(listenForClientsSocket) == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("closesocket(listenForClientsSocket)"));
	}

	// todo clientSokets.begin...delete all?

	return 0;
}

DWORD WINAPI listenForNodes(LPVOID lpParam)
{
	int iResult;
	int connectionCounter = 0;
	int acceptedPeerNodeId = -1;
	SOCKET acceptedNodeSocket = INVALID_SOCKET;

	// socket is already binded in preparing endpoint function
	iResult = listen(listenForNodesSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("listenForNodes.listen"));
		if (closesocket(listenForNodesSocket) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForNodes.listen.closesocket(listenForNodesSocket)"));
		}
		exitSignal = true;
		return 1;
	}

	printf("\n\n-----NODE: Nodes Server initialized\n");

	while (true)
	{
		SOCKADDR_IN connectedNodeAddr;
		int addrlen = sizeof(connectedNodeAddr);
		FD_SET readSet;
		FD_ZERO(&readSet);
		FD_SET(listenForNodesSocket, &readSet);
		timeval timeVal;
		timeVal.tv_sec = 1;
		timeVal.tv_usec = 0;

		iResult = select(listenForNodesSocket, &readSet, NULL, NULL, &timeVal);
		if (iResult == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForNode.select"));
			break;
		}

		if (exitSignal)
			break;

		else if (FD_ISSET(listenForNodesSocket, &readSet))
		{
			acceptedNodeSocket = accept(listenForNodesSocket, (SOCKADDR*)&connectedNodeAddr, &addrlen);
			connectionCounter++;
			//printf("\n\nAccepted Nodes - connection counter = %d", connectionCounter);
			if (acceptedNodeSocket == INVALID_SOCKET)
			{
				iResult = WSAGetLastError();
				ErrorHandlerTxt(TEXT("listenForNodes.accept"));

				if (iResult == WSAECONNRESET || iResult == WSAECONNABORTED)
					continue;

				break;
			}

			//int port = ntohs(connectedNodeAddr.sin_port);
			//char conNodeAddr[INET_ADDRSTRLEN];
			//inet_ntop(AF_INET, &(connectedNodeAddr.sin_addr), conNodeAddr, INET_ADDRSTRLEN);
			//printf("\nNode accepted  %s:%d", conNodeAddr, port);

			if (SetSocketToNonBlocking(&acceptedNodeSocket) == SOCKET_ERROR)
			{
				if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForNodes.SetSocketToNonBlocking-> closesocket (acceptedNodeSocket)"));
				}
				continue;
			}

			Message rcvdNodeRegistrationMsg;
			rcvdNodeRegistrationMsg.size = 0;
			if ((iResult = receiveMessage(acceptedNodeSocket, &rcvdNodeRegistrationMsg, 200, 3, 10, true)) == 0)
			{
				acceptedPeerNodeId = *(int*)rcvdNodeRegistrationMsg.payload;
				//printf("\n Node: * id * %d accepted", acceptedPeerNodeId);

				// sending my node id to peer as acknowledgment (and indication) of a registration
				nodeRegistrationMsg.msgType = Registration;
				nodeRegistrationMsg.size = sizeof(MsgType) + 4; // +4 is for size of node id which is payload				
				nodeRegistrationMsg.payload = (char*)&MyEndpointRegData.nodeId;

				if ((iResult = sendMessage(acceptedNodeSocket, &nodeRegistrationMsg, 50, 0, 50, true)) != nodeRegistrationMsg.size + 4)
				{
					printf("\n Something went wrong with sending registration reply to peer %d", acceptedPeerNodeId);
					switch (iResult)
					{
						case ENETDOWN:
						{
							printf("\n WSAENETDOWN");
							exitSignal = true;
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
						if (shutdown(acceptedNodeSocket, SD_SEND) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("shutdown(acceptedNodeSocket)"));
						}
						break;
						default:
						{
							printf("\n OTHER ERROR");
						}
						break;
					}

					if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> closesocket acceptedNodeSocket"));
					}
				}
				else
				{
					StoreSocket(&nodeSockets, &cs_NodeSockets, acceptedPeerNodeId, &acceptedNodeSocket);
					printf("\n *Connected with peer with id = %d", acceptedPeerNodeId);

					HANDLE nodeCommHandler;
					nodeCommHandler = CreateThread(NULL, 0, &getTransactionRequestAndSendReply, (LPVOID)acceptedPeerNodeId, 0, NULL);
					if (nodeCommHandler == NULL)
					{
						ErrorHandlerTxt(TEXT("CreateThread n2nCommunication failed"));
						if (shutdown(acceptedNodeSocket, SD_SEND) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("ConnectWithOnePeer >  CreateThread n2nCommunication.shutdown"));
						}
						if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("ConnectWithOnePeer >  CreateThread n2nCommunication.closesocket"));
						}
					}
					else
					{
						StoreHandle(&nodeCommunicationHandles, &cs_NodeCommHandles, acceptedPeerNodeId, &nodeCommHandler);
					}
				}
			}
			else
			{
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n Receiving registration message from node: WSAENETDOWN");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n Receiving registration message from node: WSAECONNABORTED");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n Receiving registration message from node: WSAECONNRESET");
					}
					break;
					case TIMED_OUT:
					{
						printf("\n Receiving registration message from node: TIMED_OUT");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n Receiving registration message from node: CLOSED_GRACEFULLY");
						if (shutdown(acceptedNodeSocket, SD_SEND) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> shutdown"));
						}
					}
					break;
					default:
					{
						printf("\n Receiving registration message from node: OTHER ERROR");
					}
					break;
				}

				if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> closesocket"));
				}
			}

			// if anything is allocated
			if (rcvdNodeRegistrationMsg.size > 4)
				free(rcvdNodeRegistrationMsg.payload);
		}
	}

	exitSignal = true;

	if (closesocket(listenForNodesSocket) == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("listenForNodes closesocket(listenForNodesSocket)"));
	}

	HANDLE n2nHandles[MAX_NODES_COUNT];
	int nodeHandleCount = nodeCommunicationHandles.size();
	int i = 0;
	for (auto it = nodeCommunicationHandles.begin(); it != nodeCommunicationHandles.end(); ++it)
	{
		n2nHandles[i] = it->second;
		i++;
		printf("\n n2n wait handles = %d", i);
	}

	WaitForMultipleObjects(nodeHandleCount, n2nHandles, TRUE, INFINITE);
	for (int i = 0; i < nodeHandleCount; i++)
	{
		SAFE_DELETE_HANDLE(n2nHandles[i])
	}

	printf("\n return from listenFromNodes");
	return 0;
}

/*
Handling master requests for node health state (ping message) and shutdown message.
Started when node finishes its positioning in cluster (discovering of peers and connecting them).
*/
DWORD WINAPI masterCommunicationListener(LPVOID lpParam)
{
	int iResult;
	while (!exitSignal)
	{
		Message msgFromMaster;
		msgFromMaster.size = 0;
		if ((iResult = receiveMessage(connectToMasterSocket, &msgFromMaster, 0, 3, 1, false)) == 0)
		{
			// todo problem ispadanja nekog od cvorova ili mastera dok traje transakcija

			if (msgFromMaster.msgType == ShutDown)
			{
				printf("\nShutDown signal received from master");
				// todo wait for transaction to finish first
				exitSignal = true;
			}
			else if (msgFromMaster.msgType == Ping)
			{

			}
			else
			{
				if (msgFromMaster.msgType == TransactionRequestApproved)
				{
					if (*(int*)msgFromMaster.payload == MyEndpointRegData.nodeId)
						isTransactionCoordinator = true;
					else
						isTransactionCoordinator = false;
				}
				else if (msgFromMaster.msgType == TransactionRequestRejected)
				{
					isTransactionCoordinator = false;
				}
				else if (msgFromMaster.msgType == Error)
				{
					isTransactionCoordinator = false;
				}

				ReleaseSemaphore(TransactionCoordinatorRequest_Signal, 1, NULL);
			}
		}
		else
		{
			if (iResult != TIMED_OUT)
			{
				exitSignal = true;
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n masterCommunication receive WSAENETDOWN od mastera");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n masterCommunication receive WSAECONNABORTED od mastera");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n masterCommunication receive WSAECONNRESET od mastera");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n masterCommunication receive CLOSED_GRACEFULLY ");
					}
					break;
					default:
					{
						printf("\n masterCommunication receive OTHER ERROR");
					}
					break;
				}
			}
		}

		if (msgFromMaster.size > 4)
			free(msgFromMaster.payload);
	}
	return 0;
}


#pragma endregion

// ovo nije bas koordinacija, kooridnaicja je tamo gde se salje rpepare i sve ostalo...
// ovo je samo dispatch
// ! Function associated with transaction coordinator role
DWORD WINAPI dispatchTransactionRequest(LPVOID lpParam)
{
	while (!exitSignal)
	{
		// todo change this not to be infinite but looping
		WaitForSingleObject(TransactionPhase_StartSignal, INFINITE);

		HANDLE hThrPeerTrReq[MAX_NODES_COUNT];
		DWORD dwThrPeerTrReq[MAX_NODES_COUNT];
		LPDWORD thrPeerResult[MAX_NODES_COUNT];
		// mozda je bilo moguce da iskoristis postojece niti n2n, da proveri sekudu na semaforu da li ima nesto za wr
		// pa onda opet radi read, ali ne komplikuj sada
		EnterCriticalSection(&cs_NodeSockets);
		int nodesCount = nodeSockets.size();
		int i = 0;
		for (auto it = nodeSockets.begin(); it != nodeSockets.end(); ++it)
		{
			//auto param = std::make_pair(it->first, &transactionRequestForNode);
			int nodeId = it->first;
			hThrPeerTrReq[i] = CreateThread(NULL, 0, sendTransactionRequestToNodeAndGetReply, (LPVOID)nodeId, 0, NULL);
			//hThrPeerTrReq[i] = CreateThread(NULL, 0, n2nTransactionRequest, (LPVOID)&param, 0, &dwThrPeerTrReq[i]);
			i++;
		}
		LeaveCriticalSection(&cs_NodeSockets);

		WaitForMultipleObjects(nodesCount, hThrPeerTrReq, TRUE, INFINITE);

		printf("dispatchTransactionRequest: wait for multiple objects finished");

		bool answer = 0;
		for (i = 0; i < nodesCount; i++)
		{
			// important to note -> 
			// 0 exit code is OK (true)
			// 1 exit code is NOK (false)
			printf("\n get exit thread code");
			//GetExitCodeThread(hThrPeerTrReq[i], thrPeerResult[i]);
			GetExitCodeThread(hThrPeerTrReq[i], &dwThrPeerTrReq[i]);
			//printf("\n thread exit code = %d", thrPeerResult[i]);
			//answer |= (bool)thrPeerResult[i];
			answer |= (bool)dwThrPeerTrReq[i];
		}


		printf("\n dispatchTransactionRequest answer = %d (if zero everything ok)", answer);
		TransactionPhaseResult = !answer;
		ReleaseSemaphore(TransactionPhase_FinishSignal, 1, NULL);
	}

	return 0;
}

#pragma region Node-Node communication

DWORD WINAPI ConnectWithOnePeer(LPVOID lpParam)
{
	int iResult;
	EndpointElement targetPeer = *(EndpointElement*)lpParam;

	SOCKET peerCommSocket;
	peerCommSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (peerCommSocket == INVALID_SOCKET)
	{
		ErrorHandlerTxt(TEXT("ConnectWithPeer socket"));
		return 1;
	}

	sockaddr_in serverAddress;
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_addr.s_addr = targetPeer.ipAddress;
	serverAddress.sin_port = htons(targetPeer.port);

	if (SetSocketToNonBlocking(&peerCommSocket) == SOCKET_ERROR)
	{
		printf("connectWithOnePeer.setToNonBlocking to non-blocking");
		if (closesocket(peerCommSocket) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("connectWithOnePeer.setToNonBlocking.closesocket"));
		}
		return 1;
	}

	if (connect(peerCommSocket, (SOCKADDR*)&serverAddress, sizeof(serverAddress)) == SOCKET_ERROR)
	{
		if (WSAGetLastError() != WSAEWOULDBLOCK)
		{
			ErrorHandlerTxt(TEXT("connectWithOnePeer.connect -> != WSAEWOULDBLOCK"));
			if (closesocket(peerCommSocket) == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("connectWithOnePeer.connect.closesocket"));
			}
			return 1;
		}
		else
		{
			FD_SET writeSet, errSet;
			FD_ZERO(&writeSet);
			FD_SET(peerCommSocket, &writeSet);
			FD_ZERO(&errSet);
			FD_SET(peerCommSocket, &errSet);
			timeval timeVal;
			timeVal.tv_sec = 30;
			timeVal.tv_usec = 0;

			iResult = select(0, NULL, &writeSet, &errSet, &timeVal);
			if (iResult == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("connectWithOnePeer.connect.select"));
				if (closesocket(peerCommSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("connectWithOnePeer.connect.select.closesocket"));
				}
				return 1;
			}
			else if (iResult == 0)
			{
				printf("\n Time limit expired for peer select");
				if (closesocket(peerCommSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("connectWithOnePeer.connect.select.closesocket - time expired"));
				}
				return 1;
			}
			else
			{
				if (FD_ISSET(peerCommSocket, &errSet))
				{
					DWORD errCode = 0;
					int len = sizeof(errCode);
					if (getsockopt(peerCommSocket, SOL_SOCKET, SO_ERROR, (char*)&errCode, &len) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("getsockopt"));
					}
					else
					{
						printf("\n error: %d ", errCode);
						if (errCode == WSAECONNREFUSED)
						{
							printf("\nConnection to peer refused. Possibly trying to connect to a service that is inactive.");
						}
					}

					if (closesocket(peerCommSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("ConnectWithOnePeer.select > 0 -> getsockopt.closesocket"));
					}
					return 1;
				}
			}
		}
	}

	if ((iResult = sendMessage(peerCommSocket, &nodeRegistrationMsg, 10, 3, 10, true)) != nodeRegistrationMsg.size + 4)
	{
		switch (iResult)
		{
			case ENETDOWN:
			{
				printf("\n sending registration message to peer WSAENETDOWN");
				exitSignal = true;
			}
			break;
			case WSAECONNRESET:
			{
				printf("\n sending registration message to peer WSAECONNRESET");
			}
			break;
			case WSAECONNABORTED:
			{
				printf("\n sending registration message to peer WSAECONNABORTED");
			}
			break;
			case TIMED_OUT:
			{
				printf("\n sending registration message to peer TIMED_OUT");
			}
			break;
			case CLOSED_GRACEFULLY:
			{
				printf("\n sending registration message to peer CLOSED_GRACEFULLY");
				if (shutdown(peerCommSocket, SD_SEND) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("ConnectWithOnePeer >  sendMessage.shutdown"));
				}
			}
			break;
			default:
			{
				printf("\n sending registration message to peer OTHER ERROR");
			}
			break;

			if (closesocket(peerCommSocket) == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("ConnectWithOnePeer >  sendMessage.closesocket"));
			}
		}
		return 1;
	}
	else
	{
		Message registrationReply;
		registrationReply.size = 0;
		if ((iResult = receiveMessage(peerCommSocket, &registrationReply, 0, 5, 1, true)) != 0)
		{
			if (iResult != TIMED_OUT)
			{
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n  receiving reg reply from peer WSAENETDOWN");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n receiving reg reply from peer  WSAECONNABORTED");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n receiving reg reply from peer  WSAECONNRESET");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n receiving reg reply from peer  CLOSED_GRACEFULLY");
						if (shutdown(peerCommSocket, SD_SEND) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("ConnectWithOnePeer >  sendMessage.shutdown"));
						}
					}
					break;
					default:
					{
						printf("\n receiving reg reply from peer  OTHER ERROR");
					}
					break;
				}

				if (closesocket(peerCommSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("ConnectWithOnePeer >  receiveMessage.closesocket"));
				}
			}

			if (registrationReply.size > 4)
				free(registrationReply.payload);
			return 1;
		}
		else
		{
			switch (registrationReply.msgType)
			{
				case Registration:
				{
					int targetPeerId = *(int*)registrationReply.payload;
					StoreSocket(&nodeSockets, &cs_NodeSockets, targetPeerId, &peerCommSocket);
					printf("\n *Connected with peer with id = %d", targetPeerId);

					if (!isIntegrityUpToDate)
					{
						DoIntegrityUpdate();
					}

					HANDLE nodeCommHandler;
					nodeCommHandler = CreateThread(NULL, 0, &getTransactionRequestAndSendReply, (LPVOID)targetPeerId, 0, NULL);
					if (nodeCommHandler == NULL)
					{
						ErrorHandlerTxt(TEXT("CreateThread n2nCommunication failed"));
						if (closesocket(peerCommSocket) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("ConnectWithOnePeer >  CreateThread n2nCommunication.closesocket"));
						}
					}
					else
					{
						StoreHandle(&nodeCommunicationHandles, &cs_NodeCommHandles, targetPeerId, &nodeCommHandler);
					}
				}
				break;
				default:
				{
					printf("\n error: regisrationReply.msgType != Registration");
				}
				break;
			}
		}

		if (registrationReply.size > 4)
			free(registrationReply.payload);
	}
	return 0;
}

/*
If node spot peer failure it simply removes it from its cluster view (peers with whom it has connection established)
*/
DWORD WINAPI getTransactionRequestAndSendReply(LPVOID lpParam)
{
	int iResult = -1;
	int help = (int)lpParam;
	int connectedPeerId = help;
	bool peerFailure = false;

	while (!exitSignal)
	{
		// akor ead ili send failuju da stavi tr online na false?

		SOCKET nodeCommSocket = GetSocket(&nodeSockets, &cs_NodeSockets, connectedPeerId);
		if (nodeCommSocket == INVALID_SOCKET)
			break;

		Message msgFromNode;
		msgFromNode.size = 0;
		if ((iResult = receiveMessage(nodeCommSocket, &msgFromNode, 0, 3, 1, false)) == 0)
		{
			Message formedTransReplyMsg;
			formedTransReplyMsg.size = 0;

			int trAnswer = (int)false;
			if (msgFromNode.msgType == PREPARE)
			{
				if (isTransactionOnLine)
				{
					// error, transaction already in process
					formedTransReplyMsg.size = sizeof(MsgType) + sizeof(Errors);
					formedTransReplyMsg.msgType = Error;
					formedTransReplyMsg.payload = (char*)&trOnLineError;
				}
				else
				{
					isTransactionOnLine = true;

					formedTransReplyMsg.size = sizeof(MsgType) + 4;
					formedTransReplyMsg.msgType = PREPARE;

					// neka logika koja odredjuje da li je oke ili ne..mozda y/n unos
					trAnswer = (int)true;
					formedTransReplyMsg.payload = (char*)&trAnswer;
				}

			}
			else if (msgFromNode.msgType == COMMIT)
			{
				// doCommit f-on

				formedTransReplyMsg.size = sizeof(MsgType) + 4;
				formedTransReplyMsg.msgType = PREPARE;

				// neka logika koja odredjuje da li je oke ili ne..mozda y/n unos
				trAnswer = (int)true;
				formedTransReplyMsg.payload = (char*)&trAnswer;
				isTransactionOnLine = false; // ?
			}
			else if (msgFromNode.msgType == ROLLBACK)
			{
				// doRollback f-on

				formedTransReplyMsg.size = sizeof(MsgType) + 4;
				formedTransReplyMsg.msgType = PREPARE;

				// neka logika koja odredjuje da li je oke ili ne..mozda y/n unos
				trAnswer = (int)true;
				formedTransReplyMsg.payload = (char*)&trAnswer;
				isTransactionOnLine = false;
			}
			else
			{
				// error, transaction already in process
				formedTransReplyMsg.size = sizeof(MsgType) + sizeof(Errors);
				formedTransReplyMsg.msgType = Error;
				formedTransReplyMsg.payload = (char*)&unknownError;
			}

			if ((iResult = sendMessage(nodeCommSocket, &formedTransReplyMsg, 100, 2, 10, false)) != formedTransReplyMsg.size + 4)
			{
				switch (iResult)
				{
					peerFailure = true;
					case ENETDOWN:
					{
						printf("\n sending trans reply to coordinator WSAENETDOWN");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n sending trans reply to coordinator WSAECONNABORTED");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n sending trans reply to coordinator WSAECONNRESET");
					}
					break;
					case TIMED_OUT:
					{
						printf("\n sending trans reply to coordinator TIMED_OUT");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n sending trans reply to coordinator CLOSED_GRACEFULLY");
					}
					break;
					default:
					{
						printf("\n OTHER ERROR iResult = %d", iResult);
						printf("\n sending trans reply to coordinator OTHER ERROR");
					}
					break;
				}
			}
		}
		else
		{
			if (iResult != TIMED_OUT)
			{
				peerFailure = true;
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n n2nTransaction_Reply WSAENETDOWN od node-a");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n n2nTransaction_Reply  WSAECONNABORTED od node-a");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n n2nTransaction_Reply WSAECONNRESET od node-a");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n n2nTransaction_Reply CLOSED_GRACEFULLY od node-a");
					}
					break;
					default:
					{
						printf("\n n2nTransaction_Reply OTHER ERROR od node-a, iResult= %d", iResult);
						ErrorHandlerTxt(TEXT("n2nTr_Reply OTHER ERROR"));

					}
					break;
				}
			}
		}

		if (msgFromNode.size > 4)
			free(msgFromNode.payload);

		if (peerFailure)
		{
			printf("\nDelete of failed peer..");
			RemoveSocket(&nodeSockets, &cs_NodeSockets, connectedPeerId);
			if (shutdown(nodeCommSocket, SD_SEND) == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("shutdown(n2n nodeCommSocket)"));
			}
			if (closesocket(nodeCommSocket) == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("closesocket(n2n nodeCommSocket)"));
			}
			break;
		}
	}
	return 0;
}

// simulate 2pc by y/n answers from nodes...
DWORD WINAPI sendTransactionRequestToNodeAndGetReply(LPVOID lpParam)
{
	int retVal = 1;
	int nodeId = (int)lpParam;
	// return 1 if answer is NO, 
	// return 0 if answer is YES
	SOCKET nodeSocket;
	nodeSocket = GetSocket(&nodeSockets, &cs_NodeSockets, nodeId);
	if (nodeSocket == INVALID_SOCKET)
	{
		printf("\n error: sendTransactionRequestToNode nodeSocket == INVALID_SOCKET");
		retVal = 1;
	}
	else
	{
		Message *trRequest = &transactionRequestForNode;
		int iResult = 0;
		if ((iResult = sendMessage(nodeSocket, trRequest, 10, 3, 3, false)) != trRequest->size + 4)
		{
			switch (iResult)
			{
				case ENETDOWN:
				{
					printf("\n sendTransactionRequestToNode WSAENETDOWN");
					exitSignal = true;
				}
				break;
				case WSAECONNRESET:
				{
					printf("\n sendTransactionRequestToNode WSAECONNRESET");
				}
				break;
				case WSAECONNABORTED:
				{
					printf("\n sendTransactionRequestToNode WSAECONNABORTED");
				}
				break;
				case TIMED_OUT:
				{
					printf("\n sendTransactionRequestToNode TIMED_OUT");
				}
				break;
				case CLOSED_GRACEFULLY:
				{
					printf("\n sendTransactionRequestToNode CLOSED_GRACEFULLY");
				}
				break;
				default:
				{
					ErrorHandlerTxt(TEXT("sendTransactionRequestToNode OTHER ERROR"));
					printf("\n sendTransactionRequestToNode OTHER ERROR");
				}
				break;
			}

			retVal = 1;
		}
		else
		{
			printf("\n Transaction request sent successfully to node %d", nodeId);
			Message trNodeReply;
			trNodeReply.size = 0;
			if ((iResult = receiveMessage(nodeSocket, &trNodeReply, 0, 3, 1, false)) == 0)
			{
				printf("\n transaction request received drom other coordinator");
				if (trNodeReply.msgType == PREPARE || trNodeReply.msgType == COMMIT || trNodeReply.msgType == ROLLBACK)
				{
					if (*(int*)trNodeReply.payload == 1)
						retVal = 0;
				}
				else if (trNodeReply.msgType == Error)
				{
					retVal = 1;
				}
				else
				{
					retVal = 1;
				}

			}
			else
			{
				retVal = 1;
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n n2ntransaction_RequestFromCoordinator WSAENETDOWN od node-a");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n n2ntransaction_RequestFromCoordinator  WSAECONNABORTED od node-a");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n n2ntransaction_RequestFromCoordinator WSAECONNRESET od node-a");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n n2ntransaction_RequestFromCoordinator CLOSED_GRACEFULLY od node-a");
					}
					break;
					case TIMED_OUT:
					{
						printf("\n n2ntransaction_RequestFromCoordinator TIMED_OUT od node-a");
					}
					break;
					default:
					{
						printf("\n n2ntransaction_RequestFromCoordinator OTHER ERROR od node-a");
					}
					break;
				}
			}

			if (trNodeReply.size > 4)
				free(trNodeReply.payload);

			printf("\n n2nTransactionRequst returning %d", retVal);
		}
	}
	return retVal;
}

#pragma endregion

#pragma region Node-Client communication

// todo integrity update

DWORD WINAPI clientCommunication(LPVOID lpParam)
{
	int iResult = -1;
	SOCKET clientCommSocket = (SOCKET)lpParam;
	/*

	here should start handling clients by making different thread per each client
	faster maybe would be to check on all sockets for readability...
	*/

	while (!exitSignal)
	{
		requestFromClient.size = 0;
		if ((iResult = receiveMessage(clientCommSocket, &requestFromClient, 0, 3, 1, false)) == 0)
		{
			printf("\n Received message from client");
			ClientMessageHeader request = *((ClientMessageHeader*)requestFromClient.payload);

			Message formedTransReplyMsg;
			formedTransReplyMsg.size = 0;

			if (isTransactionOnLine)
			{
				// error, transaction already in process
				formedTransReplyMsg.size = sizeof(MsgType) + sizeof(Errors);
				formedTransReplyMsg.msgType = Error;
				formedTransReplyMsg.payload = (char*)&trOnLineError;
			}
			else
			{
				if (SendRequestForTransactionCoordinatorRole())
				{
					printf("\n Request for getting Transaction Manager Role successfully sent to master.");
					// todo check if this is ok timeval 
					WaitForSingleObject(TransactionCoordinatorRequest_Signal, 5000);

					printf("\n Reply for getting Transaction Manager Role request");
					if (isTransactionCoordinator)
					{
						// now this node is TransactionCoordnitor
						isTransactionOnLine = true;

						printf("\nTransaction is On Line");
						if (requestFromClient.msgType == Data)
						{
							// prepare
							transactionRequestForNode.size = 0;
							transactionRequestForNode.msgType = PREPARE;
							transactionRequestForNode.size += 4;
							transactionRequestForNode.payload = (char*)&request;
							transactionRequestForNode.size += sizeof(ClientMessageHeader);

							ReleaseSemaphore(TransactionPhase_StartSignal, 1, NULL);
							WaitForSingleObject(TransactionPhase_FinishSignal, INFINITE);

							if (TransactionPhaseResult == false)
							{
								printf("\n transaction prepare result == false");
							}
							else
							{
								// commit
								printf("\n transaction prepare result == true");
							}
						}
						else
						{
							// error
							printf("\n error: transaction requestFromClien.msgType != Data");
						}
					}
					else
					{
						printf("\n transaction is already in progress. waiting for request for transaction from coordinator");
						// error, transaction already in process
						formedTransReplyMsg.size = sizeof(MsgType) + sizeof(Errors);
						formedTransReplyMsg.msgType = Error;
						formedTransReplyMsg.payload = (char*)&trOnLineError;
					}
				}
				else
				{
					// error, transaction already in process
					printf("\n transaction is already in progress.");
					formedTransReplyMsg.size = sizeof(MsgType) + sizeof(Errors);
					formedTransReplyMsg.msgType = Error;
					formedTransReplyMsg.payload = (char*)&trOnLineError;
				}
			}

			// answer to client
		}
	}
	return 0;
}

#pragma endregion

#pragma endregion

#pragma region Functions

void InitializeCriticalSections()
{
	InitializeCriticalSection(&cs_NodeSockets);
	InitializeCriticalSection(&cs_NodeCommHandles);
	InitializeCriticalSection(&cs_ClientSockets);
}

void DeleteCriticalSections()
{
	printf("\nDelete critical sections");
	DeleteCriticalSection(&cs_NodeSockets);
	DeleteCriticalSection(&cs_NodeCommHandles);
}

/*
Validation of entered port and attempt to bind. If succeed returns 0. Otherwise 1.
*/
int PrepareSocketEndpoint(SOCKET* socket, USHORT* port, const char* endpointId)
{
	int iResult = -1;
	int tempPort;

	while (!exitSignal)
	{
		printf("\nPort: %s - Please enter port number [20000-65535]: ", endpointId);
		scanf("%d", &tempPort);
		while ((getchar()) != '\n'); // flushes the standard input -> (clears the input buffer)

		if (tempPort < 20000 || tempPort > 65535)
			printf("Invalid port number. Please specify port in range 20000-65535.");

		else
		{
			*port = tempPort;
			char portStr[6];
			sprintf(portStr, "%hu", *port);

			if ((iResult = bindSocket(socket, portStr)) != 0)
			{
				if (iResult == WSAEADDRINUSE)
				{
					// expeceted behaviour in case of unsuccessfull binding is termination
					char answer = 'x';

					printf("Port %s is already used.", portStr);
					printf("\nPress y/Y for keep trying, or any other key for exit. ");
					scanf("%c", &answer);

					// if user decided to try with other port number
					if (answer == 'y' || answer == 'Y')
						continue;
				}

				exitSignal = true;
				iResult = 1;
				break;
			}

			else // binding sucessfull
				break;
		}
	};
	return iResult;
}

/*
Sending registration message to master, and receiving registration reply. If succeed returns 0. Otherwise 1.
*/
int JoinCluster(NodeRegData* registrationData, Message* registrationReply) // todo registration data unused
{
	int iResult = -1;

	nodeRegistrationMsg.msgType = Registration;
	nodeRegistrationMsg.size += sizeof(MsgType);
	nodeRegistrationMsg.size += sizeof(MyEndpointRegData);
	nodeRegistrationMsg.payload = (char*)&MyEndpointRegData;

	if ((iResult = sendMessage(connectToMasterSocket, &nodeRegistrationMsg, 100, 0, INFINITE_ATTEMPT_NO, true)) != nodeRegistrationMsg.size + 4)
	{
		if (iResult == TIMED_OUT)
			printf("\n sending registration message to master - TIME_OUT");
		if (iResult != TIMED_OUT)
		{
			switch (iResult)
			{
				case ENETDOWN:
				{
					printf("\n sending registration message to master - WSAENETDOWN");
				}
				break;
				case WSAECONNABORTED:
				{
					printf("\n sending registration message to master - WSAECONNABORTED");
				}
				break;
				case WSAECONNRESET:
				{
					printf("\n sending registration message to master - WSAECONNRESET");
				}
				break;
				case CLOSED_GRACEFULLY:
				{
					printf("\n sending registration message to master - CLOSED_GRACEFULLY");
				}
				break;
				default:
				{
					printf("\n sending registration message to master - OTHER ERROR"); // maybe WSAENOBUFS - 2000 max
				}
				break;
			}
		}
		return 1;
	}
	else
	{
		//printf("\n===Registration message (bytes=%d) sent successfully to master, node will try to get reply...",iResult);
		if ((iResult = receiveMessage(connectToMasterSocket, registrationReply, 200, 0, 200, true)) != 0)
		{
			if (iResult == TIMED_OUT)
				printf("\n receiving registration reply from master - TIME_OUT");
			if (iResult != TIMED_OUT)
			{
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n receiving registration reply from master - WSAENETDOWN");
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n receiving registration reply from master -  WSAECONNABORTED");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n receiving registration reply from master -  WSAECONNRESET");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n receiving registration reply from master -  CLOSED_GRACEFULLY");
					}
					break;
					default:
					{
						printf("\n receiving registration reply from master -  OTHER ERROR");
					}
					break;
				}
			}
			return 1;
		}

	}
	return 0;
}

/*
Connecting with all peers, if possible. Returns when all independent connection attempts to distinct peers returns.
*/
void ConnectWithPeers(char * peersInfo, int peersCount)
{
	// prepare reg data, for all nodes same data
	nodeRegistrationMsg.msgType = Registration;
	nodeRegistrationMsg.size = sizeof(MsgType) + 4; // +4 is for size of node id which is payload
	nodeRegistrationMsg.payload = (char*)&MyEndpointRegData.nodeId;

	HANDLE hThrPeerConnectArray[MAX_NODES_COUNT];
	DWORD dwThrPeerConnectArray[MAX_NODES_COUNT];

	for (int i = 0; i < peersCount; i++)
	{
		hThrPeerConnectArray[i] = CreateThread(NULL, 0, ConnectWithOnePeer, (EndpointElement*)(peersInfo + i * sizeof(EndpointElement)), 0, &dwThrPeerConnectArray[i]);
	}

	WaitForMultipleObjects(peersCount, hThrPeerConnectArray, TRUE, INFINITE);
	for (int i = 0; i < peersCount; i++)
	{
		SAFE_DELETE_HANDLE(hThrPeerConnectArray[i])
	}
	return;
}

void DoIntegrityUpdate()
{

	// todo: if this is first node in cluster - mark it as already up to date
	// here should be longer timeput because there is a possibility of joining cluster while
	// transaction is on line

}

bool SendRequestForTransactionCoordinatorRole()
{
	int iResult;
	bool retVal = false;
	Message request_ForTRCoordinator;
	request_ForTRCoordinator.size = 0;
	request_ForTRCoordinator.msgType = TransactionRequest;
	request_ForTRCoordinator.size = 8;
	request_ForTRCoordinator.payload = (char*)&MyEndpointRegData.nodeId;
	if ((iResult = sendMessage(connectToMasterSocket, &request_ForTRCoordinator, 10, 4, 2, false)) != request_ForTRCoordinator.size + 4)
	{
		switch (iResult)
		{
			case ENETDOWN:
			{
				printf("\n GetTransactionCoordinatorRole send WSAENETDOWN");
				exitSignal = true;
			}
			break;
			case WSAECONNABORTED: // master failed
			{
				printf("\n GetTransactionCoordinatorRole send WSAECONNABORTED");
			}
			break;
			case WSAECONNRESET:
			{
				printf("\n GetTransactionCoordinatorRole send WSAECONNRESET");
			}
			break;
			case TIMED_OUT:
			{
				printf("\n GetTransactionCoordinatorRole send TIMED_OUT");
			}
			break;
			case CLOSED_GRACEFULLY:
			{
				printf("\n GetTransactionCoordinatorRole send CLOSED_GRACEFULLY");
			}
			break;
			default:
			{
				printf("\n GetTransactionManagerRole send OTHER ERROR");
			}
			break;
		}

		retVal = false;
	}
	else
		retVal = true;

	return retVal;
}

#pragma endregion
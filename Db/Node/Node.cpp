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
#include "CustomLinkedList.h"

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
void DoIntegrityUpdate(SOCKET* peerSocket);
void FormDataMessage(LinkedList* storage, Message* formedMessage);
void ProcessIntegrityDataMessage(LinkedList* storage, Message* dataMessage);
bool SendRequestForTransactionCoordinatorRole();
void FinishTransaction();

DWORD WINAPI ConnectWithOnePeer(LPVOID lpParam);
DWORD WINAPI masterCommunicationListener(LPVOID lpParam);
DWORD WINAPI listenForNodes(LPVOID lpParam);
DWORD WINAPI listenForClients(LPVOID lpParam);

DWORD WINAPI ClientListener(LPVOID lpParam);
DWORD WINAPI dispatchTransactionRequest(LPVOID lpParam);
DWORD WINAPI SendTransactionRequestToNode(LPVOID lpParam);
DWORD WINAPI nodeListener(LPVOID lpParam);

std::atomic<bool> exitSignal(false);
std::atomic<bool> IsIntegrityUpToDate(false);
std::atomic<bool> IsIntegrityRequestInProgress(false);
char integrityData[DEFAULT_BUFLEN];
char helpBuff[DEFAULT_BUFLEN];

// 2PC
//-------------------------------------------------
Errors trOnLineError = TRANSACTION_ONLINE;
Errors unknownError = UNKNOWN;
std::atomic<bool> IsTransactionOnLine(false);
std::atomic<bool> IsTransactionCoordinator(false);
std::atomic<int> CurrentCoordinatorId(-1);

HANDLE CoordinationSignal;
std::atomic<int> numberOfTransactionRequestsSent(0);
std::atomic<int> numberOfReceivedTransactionReplies(0);
HANDLE Prepare_StartSignal;
HANDLE Prepare_FinishSignal;
HANDLE Commit_StartSignal;
HANDLE Commit_FinishSignal;
HANDLE Rollback_StartSignal;
HANDLE Rollback_FinishSignal;
std::atomic<int> previousPhaseId(-1);
std::atomic<int> currentPhaseId(-1); // 0 = prepare, 1 = commit, 2 = rollback
std::atomic<bool> TransactionPhaseResult(true);
Message transactionRequestForNode;
//---------------------------------------------------

SOCKET connectToMasterSocket = INVALID_SOCKET;
SOCKET listenForNodesSocket = INVALID_SOCKET;
SOCKET listenForClientsSocket = INVALID_SOCKET;

LinkedList storage;

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

	CoordinationSignal = CreateSemaphore(0, 0, 1, NULL);
	Prepare_StartSignal = CreateSemaphore(0, 0, 1, NULL);
	Prepare_FinishSignal = CreateSemaphore(0, 0, 1, NULL);
	Commit_StartSignal = CreateSemaphore(0, 0, 1, NULL);
	Commit_FinishSignal = CreateSemaphore(0, 0, 1, NULL);
	Rollback_StartSignal = CreateSemaphore(0, 0, 1, NULL);
	Rollback_FinishSignal = CreateSemaphore(0, 0, 1, NULL);

	InitializeCriticalSections();
	InitList(&storage);

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
		}
	}

	WaitForMultipleObjects(awaitNo, &eventHandles[0], true, INFINITE);

	if (connectToMasterSocket != INVALID_SOCKET)
	{
		if (shutdown(connectToMasterSocket, SD_BOTH) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("main.shutdown(connectToMasterSocket)"));
		}
	}

	if (connectToMasterSocket != INVALID_SOCKET)
	{
		if (closesocket(connectToMasterSocket) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("main.closesocket(connectToMasterSocket)"));
		}
	}

	DeleteCriticalSections();

	SAFE_DELETE_HANDLE(CoordinationSignal);
	SAFE_DELETE_HANDLE(Prepare_StartSignal);
	SAFE_DELETE_HANDLE(Prepare_FinishSignal);
	SAFE_DELETE_HANDLE(Commit_StartSignal);
	SAFE_DELETE_HANDLE(Commit_FinishSignal);
	SAFE_DELETE_HANDLE(Rollback_StartSignal);
	SAFE_DELETE_HANDLE(Rollback_FinishSignal);

	SAFE_DELETE_HANDLE(nodesListener_Handle)
	SAFE_DELETE_HANDLE(masterListener_Handle);
	SAFE_DELETE_HANDLE(clientsListener_Handle);

	WSACleanup();

	ClearList(&storage);
	//printf("\nPress any key to exit...");
	//scanf_s("%d");
	//getc(stdin);

	printf("\nExiting...");
	Sleep(2000);
	fflush(stdin);
	getchar();
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

	HANDLE transactionDispatcher = CreateThread(NULL, 0, dispatchTransactionRequest, NULL, 0, NULL);
	if (transactionDispatcher == NULL)
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
			clientCommHandler = CreateThread(NULL, 0, &ClientListener, (LPVOID)acceptedClientSocket, 0, NULL);
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

	WaitForSingleObject(transactionDispatcher, INFINITE);

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
					nodeCommHandler = CreateThread(NULL, 0, &nodeListener, (LPVOID)acceptedPeerNodeId, 0, NULL);
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
		//printf("\n n2n wait handles = %d", i);
	}

	WaitForMultipleObjects(nodeHandleCount, n2nHandles, TRUE, INFINITE);
	for (int i = 0; i < nodeHandleCount; i++)
	{
		SAFE_DELETE_HANDLE(n2nHandles[i])
	}

	//printf("\n return from listenFromNodes");
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
		msgFromMaster.msgType = Ping;
		if ((iResult = receiveMessage(connectToMasterSocket, &msgFromMaster, 0, 3, 1, false)) == 0)
		{
			if (msgFromMaster.msgType == ShutDown)
			{
				printf("\nShutDown signal received from master");
				exitSignal = true;
			}
			else if (msgFromMaster.msgType == Ping)
			{

			}
			else
			{
				if (msgFromMaster.msgType == TransactionCoordinatorRequestApproved)
				{
					if (*(int*)msgFromMaster.payload == MyEndpointRegData.nodeId)
					{
						CurrentCoordinatorId = MyEndpointRegData.nodeId;
						IsTransactionCoordinator = true;
					}
					else
						IsTransactionCoordinator = false;
				}
				else if (msgFromMaster.msgType == TransactionCoordinatorRequestRejected)
				{
					IsTransactionCoordinator = false;
				}
				else if (msgFromMaster.msgType == Error)
				{
					IsTransactionCoordinator = false;
				}

				ReleaseSemaphore(CoordinationSignal, 1, NULL);
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

DWORD WINAPI dispatchTransactionRequest(LPVOID lpParam)
{
	HANDLE hSemaphores[3] = { Prepare_StartSignal, Commit_StartSignal, Rollback_StartSignal };
	int iResult;
	while (!exitSignal)
	{
		iResult = WaitForMultipleObjects(3, hSemaphores, FALSE, 5000);
		if (iResult == WAIT_TIMEOUT)
			continue;
		if (iResult == WAIT_FAILED)
		{
			ErrorHandlerTxt(TEXT("dispatch transaction request WAIT_FAILED "));
			break;
		}
		if (iResult == WAIT_OBJECT_0 || iResult == WAIT_OBJECT_0 + 1 || iResult == WAIT_OBJECT_0 + 2)
		{
			int help = currentPhaseId;
			printf("\n Dispatching transaction request for phase %d ", help);
			HANDLE hThrPeerTrReq[MAX_NODES_COUNT];
			DWORD dwThrPeerTrReq[MAX_NODES_COUNT];
			EnterCriticalSection(&cs_NodeSockets);
			int nodesCount = nodeSockets.size();
			int i = 0;
			for (auto it = nodeSockets.begin(); it != nodeSockets.end(); ++it)
			{
				int nodeId = it->first;
				hThrPeerTrReq[i] = CreateThread(NULL, 0, SendTransactionRequestToNode, (LPVOID)nodeId, 0, NULL);
				i++;
				numberOfTransactionRequestsSent++;
			}
			LeaveCriticalSection(&cs_NodeSockets);

			if (nodesCount != 0)
			{
				//printf("\n Dispatching -> Nodes count != 0");
				WaitForMultipleObjects(nodesCount, hThrPeerTrReq, TRUE, INFINITE);
				int numberOfRequestsSent_Help = numberOfTransactionRequestsSent;

				bool answer = 0;
				for (i = 0; i < nodesCount; i++)
				{
					// important to note -> 
					// 0 exit code is OK (true)
					// 1 exit code is NOK (false)
					GetExitCodeThread(hThrPeerTrReq[i], &dwThrPeerTrReq[i]);
					answer |= (bool)dwThrPeerTrReq[i];
				}

				//printf("\n ***dispatchTransactionRequest answer = %d (if zero everything ok)", answer);
			}
			else
			{
				if (currentPhaseId == 0)
				{
					ReleaseSemaphore(Prepare_FinishSignal, 1, NULL);
				}
				else if (currentPhaseId == 1)
				{
					ReleaseSemaphore(Commit_FinishSignal, 1, NULL);
				}
				else if (currentPhaseId == 2)
				{
					ReleaseSemaphore(Rollback_FinishSignal, 1, NULL);
				}
			}
		}
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

					if (!IsIntegrityUpToDate && !IsIntegrityRequestInProgress)
					{
						DoIntegrityUpdate(&peerCommSocket);
					}

					HANDLE nodeCommHandler;
					nodeCommHandler = CreateThread(NULL, 0, &nodeListener, (LPVOID)targetPeerId, 0, NULL);
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

DWORD WINAPI nodeListener(LPVOID lpParam)
{
	int iResult = -1;
	int help = (int)lpParam;
	int connectedPeerId = help;
	bool peerFailure = false;


	while (!exitSignal)
	{
		SOCKET nodeCommSocket = GetSocket(&nodeSockets, &cs_NodeSockets, connectedPeerId);
		if (nodeCommSocket == INVALID_SOCKET)
			break;

		Message msgFromNode;
		msgFromNode.size = 0;
		if ((iResult = receiveMessage(nodeCommSocket, &msgFromNode, 0, 3, 1, false)) == 0)
		{
			//printf("\n Message received from node %d", connectedPeerId);
			Message formedReplyForNode;
			formedReplyForNode.size = 0;

			int trAnswer = (int)false;
			bool shouldSendReplyMsg = false;

			// COHORT (received request from coordinator)
			if (msgFromNode.msgType == PREPARE_REQUEST)
			{
				Message clientMessage = *((Message*)msgFromNode.payload);
				ClientMessageHeader requestHeader = *((ClientMessageHeader*)(msgFromNode.payload + 8));
				//------------------COHORT PHASE: PREPARE

				shouldSendReplyMsg = true;
				printf("\n\n COHORT: received PREPARE_REQUEST");

				if (IsTransactionOnLine)
				{
					printf("\n error: transaction already On Line");
					formedReplyForNode.size = sizeof(MsgType) + sizeof(Errors);
					formedReplyForNode.msgType = Error;
					formedReplyForNode.payload = (char*)&trOnLineError;
				}
				else
				{
					CurrentCoordinatorId = connectedPeerId;

					formedReplyForNode.size = sizeof(MsgType) + 4;
					formedReplyForNode.msgType = PREPARE_REPLY;

					switch (requestHeader.reqType)
					{
						case WRITE:
						{
							trAnswer = (int)true;
							printf("\n   Node vote to WRITE req: PREPARE_REPLY OK");

						}break;
						case READ:
						{
							trAnswer = (int)true;
							printf("\n   Node vote to READ req: PREPARE_REPLY OK");

						}break;
						case REMOVE_ALL:
						{
							bool help = TransactionPhaseResult;
							ListNode * node;
							node = FindNodeInList(&storage, requestHeader.clientId, requestHeader.originId, requestHeader.originCounter);
							if (node != nullptr)
							{
								trAnswer = (int)true;
								printf("\n   Node vote to REMOVE req: PREPARE_REPLY OK");
							}
							else
							{
								trAnswer = (int)false;
								printf("\n   Node vote to REMOVE req: PREPARE_REPLY N-OK!");
							}


						}break;
						default:
						{
							trAnswer = (int)false;
							printf("\n   Node vote to REMOVE req: PREPARE_REPLY N-OK!");
						}
					}

					formedReplyForNode.payload = (char*)&trAnswer;
				}

			}
			else if (msgFromNode.msgType == COMMIT_REQUEST)
			{
				Message clientMessage = *((Message*)msgFromNode.payload);
				ClientMessageHeader requestHeader = *((ClientMessageHeader*)(msgFromNode.payload + 8));
				//------------------COHORT PHASE: COMMIT

				shouldSendReplyMsg = true;
				printf("\n COHORT: received COMMIT_REQUEST");

				formedReplyForNode.size = sizeof(MsgType) + 4;
				formedReplyForNode.msgType = COMMIT_REPLY;

				switch (requestHeader.reqType)
				{
					case WRITE:
					{
						trAnswer = (int)true;
						printf("\n   Node vote to WRITE req: COMMIT_REPLY OK");

						Message msgToStore = *((Message*)(msgFromNode.payload));
						char* help = (msgFromNode.payload);
						msgToStore.payload = help + 8;
						
						/*
							case when data is encapsulated in this manner:
							message =>	|int size|MsgType msgType|char *payload1| (typeof Message)
							payload1 =>	|int size|MsgType msgType|char *payload2| (typeof Message)
							payload2 =>	|int clientId|int originId| int originCounter| TransactionRequestType reqType| (typeog ClientMessageHeader)
									+	"concrete payload blah blah..."
							
							e.g.
											|0+4+sizeof(payload1data)+4	| PREPARE_REQUEST|	payload1data|
							payload1data=		|0+4+sizeof(ClientMessageHeader)+4 + strlen(enterdData)|Data|payload2data|
							payload2data=		|3|-1|-1|WRITE|
										+	"Miljana"
						*/
						StoreMessage(&storage, &msgToStore, false);

					}break;
					case READ:
					{
						trAnswer = (int)true;
						printf("\n   Node vote to READ req: COMMIT_REPLY OK");

					}break;
					case REMOVE_ALL:
					{
						bool help = TransactionPhaseResult;
						ListNode * node;
						node = FindNodeInList(&storage, requestHeader.clientId, requestHeader.originId, requestHeader.originCounter);
						if (node != nullptr)
						{
							trAnswer = (int)true;
							printf("\n   Node vote to REMOVE_ALL req: COMMIT_REPLY OK");
						}
						else
						{
							trAnswer = (int)false;
							printf("\n   Node vote to REMOVE_ALL req: COMMIT_REPLY NOK!");
						}


					}break;
					default:
					{
						trAnswer = (int)false;
						printf("\n   Node vote to default req: COMMIT_REPLY NOK!");
					}
				}

				formedReplyForNode.payload = (char*)&trAnswer;
			}
			else if (msgFromNode.msgType == ROLLBACK_REQUEST)
			{
				Message clientMessage = *((Message*)msgFromNode.payload);
				ClientMessageHeader requestHeader = *((ClientMessageHeader*)(msgFromNode.payload + 8));
				//------------------COHORT PHASE: ROLLBACK

				shouldSendReplyMsg = true;
				printf("\n COHORT: received ROLLBACK_REQUEST");

				formedReplyForNode.size = sizeof(MsgType) + 4;
				formedReplyForNode.msgType = ROLLBACK_REPLY;

				switch (requestHeader.reqType)
				{
					case WRITE:
					{
						trAnswer = (int)true;
						printf("\n   Node vote to WRITE req: ROLLBACK_REPLY OK");

					}break;
					case READ:
					{
						trAnswer = (int)true;
						printf("\n   Node vote to READ req: ROLLBACK_REPLY OK");

					}break;
					case REMOVE_ALL:
					{
						bool help = TransactionPhaseResult;
						ListNode * node;
						node = FindNodeInList(&storage, requestHeader.clientId, requestHeader.originId, requestHeader.originCounter);
						if (node != nullptr)
						{
							trAnswer = (int)true;
							printf("\n   Node vote to REMOVE req: ROLLBACK_REPLY OK");
						}
						else
						{
							trAnswer = (int)false;
							printf("\n   Node vote to REMOVE req: ROLLBACK_REPLY N-OK!");
						}


					}break;
					default:
					{
						trAnswer = (int)false;
						printf("\n   Node vote to default req: ROLLBACK_REPLY NOK!");
					}
				}

				formedReplyForNode.payload = (char*)&trAnswer;
			}

			// COORDINATOR (received answers form cohort)
			else if (msgFromNode.msgType == PREPARE_REPLY)
			{
				//printf("\n received PREPARE_REPLY");
				bool help = TransactionPhaseResult;
				bool nodeAnswer = *(bool*)&msgFromNode.payload;
				help = help && nodeAnswer;
				TransactionPhaseResult = help;

				int numberOfReceivedTransactionReplies_Help = numberOfReceivedTransactionReplies;
				numberOfReceivedTransactionReplies += 1;
				numberOfReceivedTransactionReplies_Help = numberOfReceivedTransactionReplies;

				// check if condition for finishing prepare phase is satisfied
				if (numberOfReceivedTransactionReplies_Help == numberOfTransactionRequestsSent)
				{
					printf("\n Signaling Prepare_FinishSignal because condition is satisfied...");
					ReleaseSemaphore(Prepare_FinishSignal, 1, NULL);
				}
			}
			else if (msgFromNode.msgType == COMMIT_REPLY)
			{
				//printf("\n received COMMIT_REPLY");
				bool help = TransactionPhaseResult;
				bool nodeAnswer = *(bool*)&msgFromNode.payload;
				help = help && nodeAnswer;
				TransactionPhaseResult = help;

				int numberOfReceivedTransactionReplies_Help = numberOfReceivedTransactionReplies;
				numberOfReceivedTransactionReplies += 1;
				numberOfReceivedTransactionReplies_Help = numberOfReceivedTransactionReplies;

				if (numberOfReceivedTransactionReplies_Help == numberOfTransactionRequestsSent)
				{
					printf("\n Signaling Commit_FinishSignal because condition is satisfied...");
					ReleaseSemaphore(Commit_FinishSignal, 1, NULL);
				}
			}
			else if (msgFromNode.msgType == ROLLBACK_REPLY)
			{
				//printf("\n received ROLLBACK_REPLY");
				bool help = TransactionPhaseResult;
				bool nodeAnswer = *(bool*)&msgFromNode.payload;
				help = help && nodeAnswer;
				TransactionPhaseResult = help;

				int numberOfReceivedTransactionReplies_Help = numberOfReceivedTransactionReplies;
				numberOfReceivedTransactionReplies += 1;
				numberOfReceivedTransactionReplies_Help = numberOfReceivedTransactionReplies;

				if (numberOfReceivedTransactionReplies_Help == numberOfTransactionRequestsSent)
				{
					printf("\n Signaling Rollback_FinishSignal...");
					ReleaseSemaphore(Rollback_FinishSignal, 1, NULL);
				}
			}

			else if (msgFromNode.msgType == Data)
			{
				shouldSendReplyMsg = true;
				printf("\n Received request for sending Data for integrity update");
				FormDataMessage(&storage, &formedReplyForNode);
			}

			else
			{
				printf("\n error: nodeListener -> received msgType in unsupported in this context");
				formedReplyForNode.size = sizeof(MsgType) + sizeof(Errors);
				formedReplyForNode.msgType = Error;
				formedReplyForNode.payload = (char*)&unknownError;
			}

			if (shouldSendReplyMsg)
			{
				if ((iResult = sendMessage(nodeCommSocket, &formedReplyForNode, 100, 2, 10, false)) != formedReplyForNode.size + 4)
				{
					printf("\n nodeListener sendMessage %d failed", connectedPeerId);
					switch (iResult)
					{
						peerFailure = true;
						case ENETDOWN:
						{
							printf("\n sending message to node WSAENETDOWN");
							exitSignal = true;
						}
						break;
						case WSAECONNABORTED:
						{
							printf("\n sending message to node WSAECONNABORTED");
						}
						break;
						case WSAECONNRESET:
						{
							printf("\n sending message to node WSAECONNRESET");
						}
						break;
						case TIMED_OUT:
						{
							printf("\n sending message to node TIMED_OUT");
						}
						break;
						case WSAENOTSOCK:
						{
							printf("\n sending message to node WSAENOTSOCK");
						}break;
						case CLOSED_GRACEFULLY:
						{
							printf("\n sending message to nodeCLOSED_GRACEFULLY");
						}
						break;
						default:
						{
							printf("\n OTHER ERROR iResult = %d", iResult);
							printf("\n sending message to node OTHER ERROR");
						}
						break;
					}
				}
			}
		}
		else
		{
			if (iResult != TIMED_OUT && iResult != WSAEWOULDBLOCK)
			{
				peerFailure = true;
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n nodeListener WSAENETDOWN od node-a");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n nodeListener  WSAECONNABORTED od node-a");
					}
					break;
					case WSAECONNRESET:
					{
						//printf("\n nodeListener WSAECONNRESET od node-a");
					}
					break;
					case WSAENOTSOCK:
					{
						printf("\n nodeListener WSAENOTSOCK od node-a");
					}break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n nodeListener CLOSED_GRACEFULLY od node-a");
					}
					break;
					default:
					{
						printf("\n nodeListener OTHER ERROR od node-a, iResult= %d", iResult);
						ErrorHandlerTxt(TEXT("nodeListener OTHER ERROR"));

					}
					break;
				}
			}
		}

		if (msgFromNode.size > 4)
			free(msgFromNode.payload);

		if (peerFailure)
		{
			if (IsTransactionOnLine && IsTransactionCoordinator)
			{
				// simulate that answer is received from this failed peer
				printf("\n Simulating received answer form failed peer...");
				numberOfReceivedTransactionReplies++;
				bool help = TransactionPhaseResult;
				help = help && false; // because of failure
				TransactionPhaseResult = help;
			}
			if (IsTransactionOnLine && CurrentCoordinatorId == connectedPeerId)
			{
				// node is COHORT currently, and failed node is coordinator...
				printf("\n Transaction variables reset. Coordinator failed.");
				IsTransactionOnLine = false;
				CurrentCoordinatorId = -1;
			}

			int numberOfTransactionRequestsSent_Help = numberOfTransactionRequestsSent;

			if (numberOfTransactionRequestsSent_Help == numberOfReceivedTransactionReplies)
			{
				if (IsTransactionOnLine && IsTransactionCoordinator)
				{
					if (currentPhaseId == 0)
					{
						printf("\n Signaling Prepare_FinishSignal because of peer failure");
						ReleaseSemaphore(Prepare_FinishSignal, 1, NULL);
					}
					else if (currentPhaseId == 1)
					{
						printf("\n Signaling Commit_FinishSignal because of peer failure");
						ReleaseSemaphore(Commit_FinishSignal, 1, NULL);
					}
					else if (currentPhaseId == 2)
					{
						printf("\n Signaling Rollback_FinishSignal because of peer failure");
						ReleaseSemaphore(Rollback_FinishSignal, 1, NULL);
					}
				}
			}

			printf("\nDelete of failed peer %d = connectedPeerId", connectedPeerId);
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

DWORD WINAPI SendTransactionRequestToNode(LPVOID lpParam)
{
	int retVal = 1;
	int nodeId = (int)lpParam;
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
					printf("\n SendTransactionRequestToNode WSAENETDOWN");
					exitSignal = true;
				}
				break;
				case WSAECONNRESET:
				{
					printf("\n SendTransactionRequestToNode WSAECONNRESET");
				}
				break;
				case WSAECONNABORTED:
				{
					printf("\n SendTransactionRequestToNode WSAECONNABORTED");
				}
				break;
				case TIMED_OUT:
				{
					printf("\n SendTransactionRequestToNode TIMED_OUT");
				}
				break;
				case CLOSED_GRACEFULLY:
				{
					printf("\n SendTransactionRequestToNode CLOSED_GRACEFULLY");
				}
				break;
				default:
				{
					ErrorHandlerTxt(TEXT("SendTransactionRequestToNode OTHER ERROR"));
				}
				break;
			}

			retVal = 1;
		}
		else
		{
			retVal = 0;
		}
	}
	return retVal;
}

#pragma endregion

#pragma region Node-Client communication

DWORD WINAPI ClientListener(LPVOID lpParam)
{
	int iResult = -1;
	SOCKET clientCommSocket = (SOCKET)lpParam;
	Message requestFromClient;

	while (!exitSignal)
	{
		requestFromClient.size = 0;
		if ((iResult = receiveMessage(clientCommSocket, &requestFromClient, 10, 5, 1, false)) == 0)
		{
			printf("\n\n Received message from client");

			if (requestFromClient.size < 4 + sizeof(ClientMessageHeader))
			{
				printf("\n error: request from client is not valid");
				continue;
			}

			ClientMessageHeader requestHeader = *((ClientMessageHeader*)requestFromClient.payload);

			Message formedReplyForClient;
			formedReplyForClient.size = 0;

			if (IsTransactionOnLine)
			{
				// error, transaction already in process
				formedReplyForClient.size = sizeof(MsgType) + sizeof(Errors);
				formedReplyForClient.msgType = Error;
				formedReplyForClient.payload = (char*)&trOnLineError;
			}
			else
			{
				if (SendRequestForTransactionCoordinatorRole())
				{
					printf("\n Request for getting Transaction Coordinator Role successfully sent to master.");
					WaitForSingleObject(CoordinationSignal, 10000);
					if (iResult == WAIT_TIMEOUT)
					{
						printf("\n Master timeout: waited for Transaction Coordinator Role reply.");
						formedReplyForClient.size = sizeof(MsgType) + sizeof(Errors);
						formedReplyForClient.msgType = Error;
						formedReplyForClient.payload = (char*)&unknownError;
					}
					if (iResult == WAIT_FAILED)
					{
						ErrorHandlerTxt(TEXT("Wait failed: Transaction Coordinator Role reply "));
						formedReplyForClient.size = sizeof(MsgType) + sizeof(Errors);
						formedReplyForClient.msgType = Error;
						formedReplyForClient.payload = (char*)&unknownError;
						exitSignal = true;
						break;
					}
					if (iResult == WAIT_OBJECT_0)
					{
						if (IsTransactionCoordinator)
						{
							IsTransactionOnLine = true;

							printf("\n\n--- COORDINATOR ROLE - ASSIGNED");
							if (requestFromClient.msgType == Data)
							{
								// reset variables 
								TransactionPhaseResult = true;
								numberOfTransactionRequestsSent = 0;
								numberOfReceivedTransactionReplies = 0;

								//------------ COORDINATOR PHASE: PREPARE

								currentPhaseId = 0;
								printf("\n\n Current coordinator phase: PREPARE");

								formedReplyForClient.msgType = Data;
								formedReplyForClient.size = sizeof(MsgType);

								transactionRequestForNode.size = 0;
								transactionRequestForNode.msgType = PREPARE_REQUEST;
								transactionRequestForNode.size += 4;

								memcpy(helpBuff, &requestFromClient, 8);
								memcpy(helpBuff + 8, requestFromClient.payload, requestFromClient.size - 4);
								transactionRequestForNode.payload = helpBuff;
								transactionRequestForNode.size += (4 + requestFromClient.size);

								switch (requestHeader.reqType)
								{
									case WRITE:
									{
										// in future implementations in prepare peers should check if there is enough space
										// but technically if cluster configuration is homogeneous
										// and data is replicated acrross all peers consistently - than if this one node have room for storing
										// then others also should have...

										// in this ideal case it is supposed that there is always enough space
										bool help = TransactionPhaseResult;
										help = help && true;
										TransactionPhaseResult = help;

									}break;
									case READ:
									{
										// here also will always be ok (true) return value...
										// case for reading is not cosidered in details,
										// at least stored messages - information are not critical
										// so client can just obtain the current data from target node...
										bool help = TransactionPhaseResult;
										help = help && true;
										TransactionPhaseResult = help;

									}break;
									case REMOVE_ALL:
									{
										// removal of all messages with clientId equals to id of request creator

										bool help = TransactionPhaseResult;
										help = help && true;
										TransactionPhaseResult = help;

									}break;
									default:
									{

									}
								}

								ReleaseSemaphore(Prepare_StartSignal, 1, NULL);
								WaitForSingleObject(Prepare_FinishSignal, INFINITE);
								printf("\n COORDINATOR: Prepare_FinishSignal received");
								if (TransactionPhaseResult == false)
								{
									printf("\n COORDINATOR: Transaction phase result -> PREPARE RESULT == FALSE");
									//------------ COORDINATOR PHASE: ROLLBACK

									TransactionPhaseResult = true;
									numberOfTransactionRequestsSent = 0;
									numberOfReceivedTransactionReplies = 0;

									currentPhaseId = 2; // rollback							
									printf("\n\n Current coordinator phase: ROLLBACK");
									transactionRequestForNode.size = 0;
									transactionRequestForNode.msgType = ROLLBACK_REQUEST;
									transactionRequestForNode.size += 4;

									memcpy(helpBuff, &requestFromClient, 8);
									memcpy(helpBuff + 8, requestFromClient.payload, requestFromClient.size - 4);
									transactionRequestForNode.payload = helpBuff;
									transactionRequestForNode.size += (4 + requestFromClient.size);

									// start rollback phase
									ReleaseSemaphore(Rollback_StartSignal, 1, NULL);
									WaitForSingleObject(Rollback_FinishSignal, INFINITE);
									printf("\n COORDINATOR: Rollback_FinishSignal received");

									switch (requestHeader.reqType)
									{
										case WRITE:
										{
											bool help = TransactionPhaseResult;
											help = help && true;
											TransactionPhaseResult = help;

										}break;
										case READ:
										{
											// uvek oke...
											bool help = TransactionPhaseResult;
											help = help && true;
											TransactionPhaseResult = help;

										}break;
										case REMOVE_ALL:
										{
											bool help = TransactionPhaseResult;

											ListNode * node;
											node = FindNodeInList(&storage, requestHeader.clientId, requestHeader.originId, requestHeader.originCounter);
											if (node != nullptr)
											{
												help = help && true;
												TransactionPhaseResult = help;
											}
											else
											{
												help = help && false;
												TransactionPhaseResult = help;
											}

										}break;
										default:
										{

										}
									}
								}
								else
								{
									printf("\n COORDINATOR: Transaction phase result -> PREPARE RESULT == TRUE");
									//------------ COORDINATOR PHASE: COMMIT

									// reset counters
									numberOfTransactionRequestsSent = 0;
									numberOfReceivedTransactionReplies = 0;

									currentPhaseId = 1; // commit									
									printf("\n\n Current coordinator phase: COMMIT");
									transactionRequestForNode.size = 0;
									transactionRequestForNode.msgType = COMMIT_REQUEST;
									transactionRequestForNode.size += 4;

									memcpy(helpBuff, &requestFromClient, 8);
									memcpy(helpBuff + 8, requestFromClient.payload, requestFromClient.size - 4);
									transactionRequestForNode.payload = helpBuff;
									transactionRequestForNode.size += (4 + requestFromClient.size);

									// start commit
									ReleaseSemaphore(Commit_StartSignal, 1, NULL);

									WaitForSingleObject(Commit_FinishSignal, INFINITE);
									printf("\n COORDINATOR: Commit_FinishSignal received");

									switch (requestHeader.reqType)
									{
										case WRITE:
										{
											bool help = TransactionPhaseResult;
											help = help && true;
											TransactionPhaseResult = help;

											StoreOneMessage(&storage, &requestFromClient);
										}break;
										case READ:
										{
											// uvek oke...
											bool help = TransactionPhaseResult;
											help = help && true;
											TransactionPhaseResult = help;

											FormDataMessage(&storage, &formedReplyForClient);

										}break;
										case REMOVE_ALL:
										{
											bool help = TransactionPhaseResult;
											help = help && true;
											TransactionPhaseResult = help;
											
											ClientMessageHeader requestHeader = *((ClientMessageHeader*)(requestFromClient.payload ));
											RemoveFromListByKey1(&storage, requestHeader.clientId);

											// rollback brisanja bi trebao da insertuje opet poruku koja je izbrisana
											// ako je uospte izbrisana u pretohodnoj fazi...

										}break;
										default:
										{

										}
									}
								}
							}
							else
							{
								// error
								printf("\n COORDINATOR: error: transaction requestFromClient.msgType != Data");
							}
						}
						else
						{
							printf("\n transaction is already in progress. waiting for request for transaction from coordinator");
							formedReplyForClient.size = sizeof(MsgType) + sizeof(Errors);
							formedReplyForClient.msgType = Error;
							formedReplyForClient.payload = (char*)&trOnLineError;
						}
					}
				}
				else
				{
					printf("\n Sending request for TransactionCoordinator role failed.");
					formedReplyForClient.size = sizeof(MsgType) + sizeof(Errors);
					formedReplyForClient.msgType = Error;
					formedReplyForClient.payload = (char*)&unknownError;
				}
			}

			// answer to client
			if ((iResult = sendMessage(clientCommSocket, &formedReplyForClient, 10, 4, 10, false)) != formedReplyForClient.size + 4)
			{
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n sending reply to client request WSAENETDOWN");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n  sending reply to client request WSAECONNABORTED");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n  sending reply to client request WSAECONNRESET");
					}
					break;
					case TIMED_OUT:
					{
						printf("\n  sending reply to client request TIMED_OUT");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n  sending reply to client request CLOSED_GRACEFULLY");
					}
					break;
					default:
					{
						ErrorHandlerTxt(TEXT("Sending reply to client: "));
						printf("\n  sending reply to client request OTHER ERROR");
					}
					break;
				}
			}

			if (IsTransactionOnLine && IsTransactionCoordinator)
			{
				FinishTransaction();
				IsTransactionOnLine = false;
				IsTransactionCoordinator = false;
				printf("\n--- COORDINATOR ROLE - DEASSIGNED");
			}
		}

		if (requestFromClient.size > 4)
			free(requestFromClient.payload);
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
	//printf("\nDelete critical sections");
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
int JoinCluster(NodeRegData* registrationData, Message* registrationReply) 
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
	nodeRegistrationMsg.size = sizeof(MsgType) + 4; // +4 is for size of node id which is payload in this message
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

void DoIntegrityUpdate(SOCKET* peerSocket)
{
	IsIntegrityRequestInProgress = true;
	int iResult;

	EnterCriticalSection(&cs_NodeSockets);
	int count = nodeSockets.size();
	LeaveCriticalSection(&cs_NodeSockets);

	if (count == 0)
	{
		IsIntegrityUpToDate = true;
		IsIntegrityRequestInProgress = false;
		return;
	}
	else
	{
		Message integrityUpdateRequest;
		integrityUpdateRequest.size = 0;
		integrityUpdateRequest.msgType = Data;
		integrityUpdateRequest.size += 4;

		if ((iResult = sendMessage(*peerSocket, &integrityUpdateRequest, 10, 20, 5, false)) != integrityUpdateRequest.size + 4)
		{
			printf("\nSomething went wrong with sending request for integrity update");
			switch (iResult)
			{
				case ENETDOWN:
				{
					printf("\n DoIntegrityUpdate send: WSAENETDOWN");
					exitSignal = true;
				}
				break;
				case WSAECONNRESET:
				{
					printf("\n DoIntegrityUpdate send: WSAECONNRESET");
				}
				break;
				case WSAECONNABORTED:
				{
					printf("\n DoIntegrityUpdate send: WSAECONNABORTED");
				}
				break;
				case TIMED_OUT:
				{
					printf("\n DoIntegrityUpdate send: TIMED_OUT");
				}
				break;
				case CLOSED_GRACEFULLY:
				{
					printf("\n DoIntegrityUpdate send: CLOSED_GRACEFULLY");
				}
				break;
				default:
				{
					ErrorHandlerTxt(TEXT("DoIntegrityUpdate send: other error"));
				}
				break;
			}
			IsIntegrityUpToDate = false;
		}
		else
		{
			Message receivedData;
			receivedData.size = 0;

			if ((iResult = receiveMessage(*peerSocket, &receivedData, 10, 10, 2, false)) == 0)
			{
				Message formedTransReplyMsg;
				formedTransReplyMsg.size = 0;
				if (receivedData.msgType == Data)
				{
					printf("\n DoIntegrityUpdate: Received integrity data message");
					ProcessIntegrityDataMessage(&storage, &receivedData);
				}
				else if (receivedData.msgType == Error)
				{
					printf("\n DoIntegrityUpdate: Received error message");
					IsIntegrityUpToDate = false;
				}
				else
				{
					printf("\n DoIntegrityUpdate: Received 'else' data message");
					IsIntegrityUpToDate = false;
				}

			}
			else
			{
				switch (iResult)
				{
					case WSAENOTSOCK:
					{
						// npr. ako posaljes shutdown koji pogasi sockete...dok je ovaj u receiv-u
					}
					case ENETDOWN:
					{
						//printf("\n transaction receiving WSAENETDOWN od node-a");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n transaction receiving WSAECONNABORTED od node-a");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n transaction receiving WSAECONNRESET od node-a");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n transaction receiving CLOSED_GRACEFULLY od node-a");
					}
					break;
					case TIMED_OUT:
					{
						//printf("\n transaction TIMED_OUT od node-a");
					}
					break;
					case WSAEWOULDBLOCK:
					{
						printf("\n transaction receiving WSAEWOULDBLOCK od node-a");
					}
					default:
					{
						ErrorHandlerTxt(TEXT("tr other error od node-a"));
					}
					break;
				}

				IsIntegrityUpToDate = false;
			}

			// if anything is allocated
			if (receivedData.size > 4)
				free(receivedData.payload);

			IsIntegrityRequestInProgress = false;
		}
	}
}

void FormDataMessage(LinkedList* storageList, Message* formedMessage)
{
	// integrity data is comprised of sequentially coppied content of each existing message 
	formedMessage->msgType = Data;
	formedMessage->size = sizeof(MsgType);
	int totalCountOfBytes = 0;
	ListNode* currentNode = storageList->pHead;

	EnterCriticalSection(&storageList->cs_Data);
	if (storageList->nodesCount != 0) {
		int idx = 0;
		do {
			size_t wholeCurrentMessageSize = 4 + *(int*)(currentNode->pData);
			memcpy(integrityData + idx, currentNode->pData, wholeCurrentMessageSize);
			totalCountOfBytes += wholeCurrentMessageSize;
			idx += wholeCurrentMessageSize;
		} while ((currentNode = currentNode->pNext) != nullptr);
	}
	LeaveCriticalSection(&storageList->cs_Data);

	formedMessage->size += totalCountOfBytes;
	formedMessage->payload = integrityData;
}

void ProcessIntegrityDataMessage(LinkedList* storage, Message* dataMessage)
{
	if (dataMessage->msgType != Data)
	{
		IsIntegrityUpToDate = false;
		return;
	}

	int payloadSize = dataMessage->size - 4;

	if (payloadSize == 0)
	{
		IsIntegrityUpToDate = true;
		return;
	}

	int currentlyStored = 0;
	int idx = 0;
	while (true)
	{
		if (currentlyStored == payloadSize)
			break;

		Message msgToStore = *(Message*)(dataMessage->payload + idx);
		char* help = (dataMessage->payload) + idx;
		msgToStore.payload = help + 8;

		StoreMessage(storage, &msgToStore, false);

		idx += 4;
		idx += msgToStore.size;
		currentlyStored = idx;
	}

	IsIntegrityUpToDate = true;
}

bool SendRequestForTransactionCoordinatorRole()
{
	int iResult;
	bool retVal = false;
	Message request_ForTRCoordinator;
	request_ForTRCoordinator.size = 0;
	request_ForTRCoordinator.msgType = TransactionCoordinatorRequest;
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
				exitSignal = true;
			}
			break;
			case WSAECONNRESET: // master failed
			{
				printf("\n GetTransactionCoordinatorRole send WSAECONNRESET");
				exitSignal = true;
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

void FinishTransaction()
{
	int iResult;
	bool retVal = false;
	Message finishTrMessage;
	finishTrMessage.size = 0;
	finishTrMessage.msgType = TransactionFinishedRequest;
	finishTrMessage.size = 8;
	finishTrMessage.payload = (char*)&MyEndpointRegData.nodeId;
	if ((iResult = sendMessage(connectToMasterSocket, &finishTrMessage, 10, 4, 2, false)) != finishTrMessage.size + 4)
	{
		switch (iResult)
		{
			case ENETDOWN:
			{
				printf("\n FinishTransaction send WSAENETDOWN");
				exitSignal = true;
			}
			break;
			case WSAECONNABORTED: // master failed
			{
				printf("\n FinishTransaction send WSAECONNABORTED");
				exitSignal = true;
			}
			break;
			case WSAECONNRESET: // master failed
			{
				printf("\n FinishTransaction send WSAECONNRESET");
				exitSignal = true;
			}
			break;
			case TIMED_OUT:
			{
				printf("\n FinishTransaction send TIMED_OUT");
			}
			break;
			case CLOSED_GRACEFULLY:
			{
				printf("\n FinishTransaction send CLOSED_GRACEFULLY");
			}
			break;
			default:
			{
				printf("\n FinishTransaction send OTHER ERROR");
			}
			break;
		}
	}
}

#pragma endregion
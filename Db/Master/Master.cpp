#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "User32.lib") // for msg box

#define _CRTDBG_MAP_ALLOC  
#include <stdlib.h>  
#include <crtdbg.h>  

#include "Communication.h"
#include "ClusterLifecycle.h"

#include <windows.h> 
#include <mmsystem.h> 
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <conio.h>
#include <time.h>
#include <strsafe.h>

#include <atomic>
#include <map>

#include <unordered_set>

#define PORT_FOR_CLIENTS "27019" 
#define PORT_FOR_NODES "27017"  

std::map<int, SOCKET> nodeSockets;
CRITICAL_SECTION cs_NodeSockets;

std::map<int, EndpointElement> nodesEndpoints_ForNodes;
CRITICAL_SECTION cs_NodesEndp;

std::unordered_set<int> clientIds;
CRITICAL_SECTION cs_ClientIds;

std::map<int, ExtendedEndpointElement> nodesEndpoints_ForClients;
CRITICAL_SECTION cs_ClientsEndp;

void InitializeCriticalSections();
void DeleteCriticalSections();
void formEndpointsMsg_ForNodes(Message* msgForNode, NodeRegData* connectedNodeInfo);
void formEndpointsMsg_ForClients(Message* msgForClient, int clientId);

MMRESULT hHealthCheckTimer, hTransactionFinishedTimer;

// listeners
DWORD WINAPI listenForNodes(LPVOID lpParam);
DWORD WINAPI listenForClients(LPVOID lpParam);

DWORD WINAPI userCancellationCheck(LPVOID lpParam);
DWORD WINAPI ShuttingDown(LPVOID lpParam);
void CALLBACK HealthCheck(UINT timerID, UINT msg, DWORD dwUser, DWORD dw1, DWORD dw2);

// master-node communication
DWORD WINAPI ShutDownOneNode(LPVOID lpParam);
DWORD WINAPI PingOneNode(LPVOID lpParam);
DWORD WINAPI ListenForRequestForAssignmentOfTransactionCoordinatorRole(LPVOID lpParam);

Errors nonUniqueIdError = NON_UNIQUE_ID;
Errors trOnLineError = TRANSACTION_ONLINE;
Errors nonSupportedError = NON_SUPPORTED_OPERATION;
Errors unknownError = UNKNOWN;
int NoCoordinatorId = -1;

std::atomic<bool> exitSignal(false);
std::atomic<bool> IsTransactionOnLine(false);
std::atomic<int> CurrentCoordinatorId(-1);
HANDLE ShutDownNodesSignal_Handle;

int __cdecl main(int argc, char **argv)
{
	//_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

	HANDLE clientsListener_Handle = NULL, nodesListener_Handle = NULL, cancellation_Handle = NULL, shuttingDown_Handle = NULL;
	DWORD clientsListener_Id, nodesListener_Id, cancellation_Id, shuttingDown_Id;

	DWORD currentProcId;
	currentProcId = GetCurrentProcessId();
	printf("\n*** PROCES ID= %d ****\n", currentProcId);

	if (argc != 2)
	{
		/*
			second argument is its address, maybe useful in future ->
			pinging in order to determine "health" of underlying network...(e.g. is app on the Internet)
		*/
		printf("usage: %s server-name\n", argv[0]);
		return 1;
	}

	char title[100];
	sprintf_s(title, 100, "MASTER --- %s:%s (press any key to exit)", argv[1], PORT_FOR_NODES);
	SetConsoleTitleA(title);

	if (InitializeWindowsSockets() == false)
	{
		// we won't log anything since it will be logged  by InitializeWindowsSockets() function
		return 1;
	}

	printf("\nSetting cluster up...");

	ShutDownNodesSignal_Handle = CreateSemaphore(0, 0, 1, NULL);
	InitializeCriticalSections();

	cancellation_Handle = CreateThread(NULL, 0, &userCancellationCheck, (LPVOID)0, 0, &cancellation_Id);
	if (cancellation_Handle == NULL)
	{
		ErrorHandlerTxt(TEXT("CreateThread userCancellationCheck"));
		DeleteCriticalSections();
		SAFE_DELETE_HANDLE(ShutDownNodesSignal_Handle);
		WSACleanup();

		printf("\n(cancellation thread failed) -> Press any key to exit...");
		scanf_s("%d");
		getc(stdin);

		ExitProcess(3);
	}

	shuttingDown_Handle = CreateThread(NULL, 0, &ShuttingDown, 0, 0, &shuttingDown_Id);
	if (shuttingDown_Handle == NULL)
	{
		exitSignal = true;
		ErrorHandlerTxt(TEXT("CreateThread ShuttingDown"));
		WaitForSingleObject(cancellation_Handle, INFINITE);

		DeleteCriticalSections();
		SAFE_DELETE_HANDLE(ShutDownNodesSignal_Handle);
		SAFE_DELETE_HANDLE(cancellation_Handle);
		WSACleanup();

		printf("\n(ShuttingDown thread failed) -> Press any key to exit...");
		scanf_s("%d");
		getc(stdin);

		ExitProcess(3);
	}

	nodesListener_Handle = CreateThread(NULL, 0, &listenForNodes, (LPVOID)0, 0, &nodesListener_Id);
	if (nodesListener_Handle == NULL)
	{
		exitSignal = true;
		ErrorHandlerTxt(TEXT("CreateThread listenForNodes"));

		HANDLE handles[] =
		{
			cancellation_Handle,
			shuttingDown_Handle
		};

		WaitForMultipleObjects(2, &handles[0], true, INFINITE);

		DeleteCriticalSections();
		SAFE_DELETE_HANDLE(ShutDownNodesSignal_Handle);
		SAFE_DELETE_HANDLE(cancellation_Handle);
		SAFE_DELETE_HANDLE(shuttingDown_Handle);
		WSACleanup();

		printf("\n(nodesListener thread failed) -> Press any key to exit...");
		scanf_s("%d");
		getc(stdin);

		ExitProcess(3);
	}

	Sleep(3000);

	// after some short timeout and allowing initial number of nodes to register to cluster, master start serving clients
	clientsListener_Handle = CreateThread(NULL, 0, &listenForClients, (LPVOID)0, 0, &clientsListener_Id);
	if (clientsListener_Handle == NULL)
	{
		exitSignal = true;
		ErrorHandlerTxt(TEXT("CreateThread listenForClients"));

		HANDLE handles[] =
		{
			cancellation_Handle,
			shuttingDown_Handle,
			nodesListener_Handle
		};

		WaitForMultipleObjects(3, &handles[0], true, INFINITE);

		DeleteCriticalSections();
		SAFE_DELETE_HANDLE(ShutDownNodesSignal_Handle);
		SAFE_DELETE_HANDLE(cancellation_Handle);
		SAFE_DELETE_HANDLE(shuttingDown_Handle);
		SAFE_DELETE_HANDLE(nodesListener_Handle);
		WSACleanup();

		printf("\n(clientsListener thread failed) -> Press any key to exit...");
		scanf_s("%d");
		getc(stdin);

		ExitProcess(3);
	}

	hHealthCheckTimer = timeSetEvent(NODE_PROBE_TIME, 100, HealthCheck, NULL, TIME_PERIODIC);

	HANDLE eventHandles[] =
	{
		cancellation_Handle,
		nodesListener_Handle,
		clientsListener_Handle,
		shuttingDown_Handle
	};

	WaitForMultipleObjects(sizeof(eventHandles) / sizeof(eventHandles[0]), &eventHandles[0], TRUE, INFINITE);

	DeleteCriticalSections();
	timeKillEvent(hHealthCheckTimer);
	SAFE_DELETE_HANDLE(ShutDownNodesSignal_Handle);
	SAFE_DELETE_HANDLE(cancellation_Handle);
	SAFE_DELETE_HANDLE(shuttingDown_Handle);
	SAFE_DELETE_HANDLE(nodesListener_Handle);
	SAFE_DELETE_HANDLE(clientsListener_Handle);

	WSACleanup();

	printf("\nExiting...");
	Sleep(2000);
	getchar();
	return 0;
}

#pragma region Functions

void DeleteCriticalSections()
{
	DeleteCriticalSection(&cs_NodesEndp);
	DeleteCriticalSection(&cs_ClientsEndp);
	DeleteCriticalSection(&cs_NodeSockets);
}

void InitializeCriticalSections()
{
	InitializeCriticalSection(&cs_NodesEndp);
	InitializeCriticalSection(&cs_ClientsEndp);
	InitializeCriticalSection(&cs_NodeSockets);
	InitializeCriticalSection(&cs_ClientIds);
}

void formEndpointsMsg_ForNodes(Message* msgForNode, NodeRegData* connectedNodeInfo)
{
	msgForNode->payload = NULL;

	EnterCriticalSection(&cs_NodesEndp);
	int nodesMapSize = nodesEndpoints_ForNodes.size();
	LeaveCriticalSection(&cs_NodesEndp);

	// this is first node, and empty message with only MsgType=Registration will be returned. Payload == NULL
	if (nodesMapSize == 0)
	{
		msgForNode->size = sizeof(MsgType);
		msgForNode->msgType = Registration;
	}
	else
	{
		//printf("\n ----  currently %d node is stored", nodesMapSize);
		EnterCriticalSection(&cs_NodesEndp);
		if (nodesEndpoints_ForNodes.count(connectedNodeInfo->nodeId) > 0)
		{
			LeaveCriticalSection(&cs_NodesEndp);

			printf("\n ---- NODE ALREADY EXISTS");
			msgForNode->size = sizeof(MsgType) + sizeof(Errors);
			msgForNode->msgType = Error;
			msgForNode->payload = (char*)&nonUniqueIdError;
		}
		else
		{
			size_t nodesMapSize = nodesEndpoints_ForNodes.size();
			msgForNode->size = sizeof(MsgType);
			msgForNode->msgType = Registration;

			if (!nodesEndpoints_ForNodes.empty())
			{
				int endpointsSize = nodesMapSize * sizeof(EndpointElement);
				msgForNode->size += endpointsSize;

				//printf("\n size za calloc = %d", endpointsSize);
				msgForNode->payload = (char*)calloc(endpointsSize, sizeof(char));
				//printf("\n\npoziv CALLOC, payload (address) = %x", msgForNode->payload);

				int help = 0;
				for (auto it = nodesEndpoints_ForNodes.begin(); it != nodesEndpoints_ForNodes.end(); ++it)
				{
					char * dest = (msgForNode->payload + (help * sizeof(EndpointElement)));
					char * source = (char*)(&((*it).second));
					memcpy(dest, source, sizeof(EndpointElement));
					help++;
				}
			}
			LeaveCriticalSection(&cs_NodesEndp);
		}
	}
}

void formEndpointsMsg_ForClients(Message* msgForClient, int clientId)
{
	msgForClient->payload = NULL;

	EnterCriticalSection(&cs_ClientsEndp);
	int clientsMapSize = nodesEndpoints_ForClients.size();
	LeaveCriticalSection(&cs_ClientsEndp);

	// there is still no nodes, and empty message with only MsgType=Data will be returned. Payload == NULL
	if (clientsMapSize == 0)
	{
		msgForClient->size = sizeof(MsgType);
		msgForClient->msgType = Registration;
	}
	else
	{
		//printf("\n ----  currently %d client is stored", clientsMapSize);
		EnterCriticalSection(&cs_ClientIds);
		if (clientIds.count(clientId) > 0)
		{
			LeaveCriticalSection(&cs_ClientIds);

			printf("\n ---- CLIENT ALREADY EXISTS");
			msgForClient->size = sizeof(MsgType) + sizeof(Errors);
			msgForClient->msgType = Error;
			msgForClient->payload = (char*)&nonUniqueIdError;
		}
		else
		{
			LeaveCriticalSection(&cs_ClientIds);

			EnterCriticalSection(&cs_ClientsEndp);
			size_t clientsMapSize = nodesEndpoints_ForClients.size();
			msgForClient->size = sizeof(MsgType);
			msgForClient->msgType = Registration;

			if (!nodesEndpoints_ForClients.empty())
			{
				int endpointsSize = clientsMapSize * sizeof(ExtendedEndpointElement);
				msgForClient->size += endpointsSize;

				//printf("\n size za calloc = %d", endpointsSize);
				msgForClient->payload = (char*)calloc(endpointsSize, sizeof(char));
				//printf("\n\npoziv CALLOC, payload (address) = %x", msgForClient->payload);

				int helpCounter = 0;
				for (auto it = nodesEndpoints_ForClients.begin(); it != nodesEndpoints_ForClients.end(); ++it)
				{
					char * dest = (msgForClient->payload + (helpCounter * sizeof(ExtendedEndpointElement)));
					char * source = (char*)(&((*it).second));
					memcpy(dest, source, sizeof(ExtendedEndpointElement));
					helpCounter++;
				}
			}
			LeaveCriticalSection(&cs_ClientsEndp);
		}
	}
}

#pragma endregion

#pragma region Threads

#pragma region Listener Threads
/*
Main logic for serving nodes. Responsible for node registration to database cluster.
Returns zero if everything succeed and resources have been cleaned up gracefully.
*/
DWORD WINAPI listenForNodes(LPVOID lpParam)
{
	int iResult;
	int connectionCounter = 0;
	SOCKET listenForNodesSocket = INVALID_SOCKET;
	SOCKET acceptedNodeSocket = INVALID_SOCKET;

	if ((iResult = bindSocket(&listenForNodesSocket, (char*)PORT_FOR_NODES)) != 0)
	{
		if (iResult == WSAEADDRINUSE)
			printf("\nPort %s is already used.", PORT_FOR_NODES);

		exitSignal = true;
		return 1;
	}

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

	printf("\n------MASTER: Nodes Server initialized------");

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
			ErrorHandlerTxt(TEXT("listenForNodes (blocking socket).select"));
			break;
		}

		if (exitSignal)
			break;

		if (FD_ISSET(listenForNodesSocket, &readSet))
		{
			acceptedNodeSocket = accept(listenForNodesSocket, (SOCKADDR*)&connectedNodeAddr, &addrlen);
			connectionCounter++;
			//printf("\n\nNodes connection counter = %d", connectionCounter);
			if (acceptedNodeSocket == INVALID_SOCKET)
			{
				iResult = WSAGetLastError();
				ErrorHandlerTxt(TEXT("listenForNodes.accept"));

				if (iResult == WSAECONNRESET || iResult == WSAECONNABORTED)
					continue;

				// WSAEINVAL (10022) -> listen function was not invoked prior to accept
				// WSAENETDOWN (10050) -> The network subsystem has failed (procitala sam se ova greska retko dogadja u novijim windows verzijama)
				break;
			}

			//int port = ntohs(connectedNodeAddr.sin_port);
			//char conNodeAddr[INET_ADDRSTRLEN];
			//inet_ntop(AF_INET, &(connectedNodeAddr.sin_addr), conNodeAddr, INET_ADDRSTRLEN);
			//printf("\nConnection from Node %s:%d accepted.", conNodeAddr, port);

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
				NodeRegData nodeInfo = *((NodeRegData*)rcvdNodeRegistrationMsg.payload);
				//struct in_addr nodeIpAddr;
				//nodeIpAddr.S_un.S_addr = nodeInfo.intIpAddress;
				//char *nodeIpAddStr = inet_ntoa(nodeIpAddr);
				//printf("\nNode: * id=%d *, ip=%s, portForNodes=%d, portForClients=%d accepted", nodeInfo.nodeId, nodeIpAddStr, nodeInfo.portForNodes, nodeInfo.portForClients);
				printf("\n Node: * id=%d * accepted", nodeInfo.nodeId);

				Message formedReplyMsg_ForNode;
				formEndpointsMsg_ForNodes(&formedReplyMsg_ForNode, &nodeInfo); // potential usage of calloc 

				if ((iResult = sendMessage(acceptedNodeSocket, &formedReplyMsg_ForNode, 100, 0, 100, true)) != formedReplyMsg_ForNode.size + 4)
				{
					switch (iResult)
					{
						case ENETDOWN:
						{
							printf("\n  sending reg reply to node WSAENETDOWN");
							exitSignal = true;
						}
						break;
						case WSAECONNABORTED:
						{
							printf("\n  sending reg reply to node WSAECONNABORTED");
						}
						break;
						case WSAECONNRESET:
						{
							printf("\n  sending reg reply to node WSAECONNRESET");
						}
						break;
						case TIMED_OUT:
						{
							printf("\n  sending reg reply to node TIMED_OUT");
						}
						break;
						case CLOSED_GRACEFULLY:
						{
							printf("\n  sending reg reply to node CLOSED_GRACEFULLY");
						}
						break;
						default:
						{
							printf("\n  sending reg reply to node OTHER ERROR");
						}
						break;
					}
					if (shutdown(acceptedNodeSocket, SD_SEND) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("shutdown (acceptedNodeSocket)"));
					}
					if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> closesocket acceptedNodeSocket"));
					}
				}
				else
				{
					if (formedReplyMsg_ForNode.msgType != Error)
					{
						//printf("\n Registration reply sent SUCCESSFULLY - %d registration bytes", iResult);		
						EndpointElement endpointForNode;
						endpointForNode.ipAddress = nodeInfo.intIpAddress;
						endpointForNode.port = nodeInfo.portForNodes;
						StoreEndpoint(&nodesEndpoints_ForNodes, &cs_NodesEndp, nodeInfo.nodeId, &endpointForNode);

						ExtendedEndpointElement endpointForClient;
						endpointForClient.endpointId = nodeInfo.nodeId;
						endpointForClient.ipAddress = nodeInfo.intIpAddress;
						endpointForClient.port = nodeInfo.portForClients;
						StoreEndpoint(&nodesEndpoints_ForClients, &cs_ClientsEndp, nodeInfo.nodeId, &endpointForClient);

						StoreSocket(&nodeSockets, &cs_NodeSockets, nodeInfo.nodeId, &acceptedNodeSocket);

						CreateThread(NULL, 0, &ListenForRequestForAssignmentOfTransactionCoordinatorRole, (LPVOID)nodeInfo.nodeId, 0, NULL);
					}
				}

				if (formedReplyMsg_ForNode.msgType == Registration && formedReplyMsg_ForNode.size >= 8)
					free(formedReplyMsg_ForNode.payload);
			}
			else
			{
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n Receiving registration message from node WSAENETDOWN");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n Receiving registration message from node WSAECONNABORTED");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n Receiving registration message from node WSAECONNRESET");
					}
					break;
					case TIMED_OUT:
					{
						printf("\n Receiving registration message from node TIMED_OUT");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n Receiving registration message from node CLOSED_GRACEFULLY");
					}
					break;
					default:
					{
						printf("\n Receiving registration message from node OTHER ERROR");
					}
					break;
				}

				if (shutdown(acceptedNodeSocket, SD_SEND))
				{
					ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> shutdown"));
				}
				if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> closesocket acceptedNodeSocket"));
				}
			}

			if (rcvdNodeRegistrationMsg.size > 4)
				free(rcvdNodeRegistrationMsg.payload);
		}
	}

	exitSignal = true;

	if (closesocket(listenForNodesSocket) == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("closesocket(listenForNodesSocket)"));
	}

	return 0;
}

DWORD WINAPI listenForClients(LPVOID lpParam)
{
	int iResult;
	int clientId = -1;
	int connectionCounter = 0;
	SOCKET listenForClientsSocket = INVALID_SOCKET;
	SOCKET acceptedClientSocket = INVALID_SOCKET;

	if ((iResult = bindSocket(&listenForClientsSocket, (char*)PORT_FOR_CLIENTS)) != 0)
	{
		if (iResult == WSAEADDRINUSE)
			printf("\nPort %s is already used.", PORT_FOR_CLIENTS);

		exitSignal = true;
		return 1;
	}

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

	printf("\n------MASTER: Clients Server initialized------");

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
			ErrorHandlerTxt(TEXT("listenForClients (blocking socket).select"));
			break;
		}

		if (exitSignal)
			break;

		if (FD_ISSET(listenForClientsSocket, &readSet))
		{
			acceptedClientSocket = accept(listenForClientsSocket, (SOCKADDR*)&connectedClientAddr, &addrlen);
			connectionCounter++;
			//printf("\n\nClients Connection counter = %d", connectionCounter);
			if (acceptedClientSocket == INVALID_SOCKET)
			{
				iResult = WSAGetLastError();
				ErrorHandlerTxt(TEXT("listenForClients.accept"));

				if (iResult == WSAECONNRESET || iResult == WSAECONNABORTED)
					continue;

				// WSAEINVAL (10022) -> listen function was not invoked prior to accept
				// WSAENETDOWN (10050) -> The network subsystem has failed (procitala sam se ova greska retko dogadja u novijim windows verzijama)
				break;
			}

			if (SetSocketToNonBlocking(&acceptedClientSocket) == SOCKET_ERROR)
			{
				if (closesocket(acceptedClientSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForClients.SetSocketToNonBlocking-> closesocket (acceptedClientSocket)"));
				}
				continue;
			}

			Message rcvdClientRegistrationMsg;
			rcvdClientRegistrationMsg.size = 0;
			if ((iResult = receiveMessage(acceptedClientSocket, &rcvdClientRegistrationMsg, 10, 4, 30, true)) == 0)
			{
				clientId = *(int*)rcvdClientRegistrationMsg.payload;
				printf("\n Client: * id * %d accepted", clientId);

				Message formedReplyMsg_ForClient;
				formEndpointsMsg_ForClients(&formedReplyMsg_ForClient, clientId); // potential usage of calloc here

				if ((iResult = sendMessage(acceptedClientSocket, &formedReplyMsg_ForClient, 100, 0, 100, true)) != formedReplyMsg_ForClient.size + 4)
				{
					switch (iResult)
					{
						case ENETDOWN:
						{
							printf("\n Reply message to client WSAENETDOWN");
							exitSignal = true;
						}
						break;
						case WSAECONNRESET:
						{
							printf("\n Reply message to client WSAECONNRESET");
						}
						break;
						case TIMED_OUT:
						{
							printf("\n Reply message to client TIMED_OUT");
						}
						break;
						case CLOSED_GRACEFULLY:
						{
							printf("\n Reply message to client CLOSED_GRACEFULLY");
						}
						break;
						default:
						{
							printf("\n Reply message to client OTHER ERROR");
						}
						break;
					}
					if (shutdown(acceptedClientSocket, SD_SEND) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("shutdown(acceptedClientSocket)"));
					}
					if (closesocket(acceptedClientSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("closesocket(acceptedClientSocket)"));
					}
				}
				else
				{
					EnterCriticalSection(&cs_ClientIds);
					clientIds.insert(clientId);
					LeaveCriticalSection(&cs_ClientIds);
				}

				if (formedReplyMsg_ForClient.msgType == Registration && formedReplyMsg_ForClient.size >= 8)
					free(formedReplyMsg_ForClient.payload);
			}
			else
			{
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n Receiving registration message from client WSAENETDOWN");
						exitSignal = true;
					}
					break;
					case WSAECONNABORTED:
					{
						printf("\n Receiving registration message from client WSAECONNABORTED");
					}
					break;
					case WSAECONNRESET:
					{
						printf("\n Receiving registration message from client WSAECONNRESET");
					}
					break;
					case TIMED_OUT:
					{
						printf("\n Receiving registration message from client TIMED_OUT");
					}
					break;
					case CLOSED_GRACEFULLY:
					{
						printf("\n Receiving registration message from client CLOSED_GRACEFULLY");
					}
					break;
					default:
					{
						printf("\n Receiving registration message from client OTHER ERROR");
					}
					break;
				}
				if (shutdown(acceptedClientSocket, SD_SEND))
				{
					ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> shutdown"));
				}
				if (closesocket(acceptedClientSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForClients.receiveMessage -> closesocket acceptedClientSocket"));
				}
			}

			if (rcvdClientRegistrationMsg.size > 4)
				free(rcvdClientRegistrationMsg.payload);
		}
	}

	exitSignal = true;

	if (closesocket(listenForClientsSocket) == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("closesocket(listenForClientsSocket)"));
	}

	return 0;
}

#pragma endregion

#pragma region Coordination Threads (managing cluster lifecycle)

DWORD WINAPI userCancellationCheck(LPVOID lpParam)
{
	FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));

	while (true)
	{
		if (_kbhit()) {
			char c = _getch();
			{
				exitSignal = true;
				ReleaseSemaphore(ShutDownNodesSignal_Handle, 1, NULL);
			}
			return 0;
		}
		Sleep(USER_CANCELLATION_TIME_MS);
	}
}

DWORD WINAPI ShuttingDown(LPVOID lpParam)
{
	WaitForSingleObject(ShutDownNodesSignal_Handle, INFINITE);
	HANDLE hThrNodes[MAX_NODES_COUNT];
	DWORD dwThrNodes[MAX_NODES_COUNT];

	EnterCriticalSection(&cs_NodeSockets);
	int nodesCount = nodeSockets.size();

	int i = 0;
	for (auto it = nodeSockets.begin(); it != nodeSockets.end(); ++it)
	{
		hThrNodes[i] = CreateThread(NULL, 0, ShutDownOneNode, (LPVOID)&(*it), 0, &dwThrNodes[i]);
		i++;
	}
	LeaveCriticalSection(&cs_NodeSockets);

	WaitForMultipleObjects(nodesCount, hThrNodes, TRUE, INFINITE);

	for (int i = 0; i < nodesCount; i++)
	{
		SAFE_DELETE_HANDLE(hThrNodes[i])
	}
	for (auto it = nodeSockets.begin(); it != nodeSockets.end(); ++it)
	{
		if (shutdown(it->second, SD_SEND) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("shutdown(map.nodeSocket)"));
		}
	}
	for (auto it = nodeSockets.begin(); it != nodeSockets.end(); ++it)
	{
		if (closesocket(it->second) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("closesocket(map.nodeSocket)"));
		}
	}
	nodeSockets.clear();
	return 0;
}

#pragma endregion

#pragma region Master-Node communication

// sending shut down message to node
DWORD WINAPI ShutDownOneNode(LPVOID lpParam)
{
	//printf("\n ConnectWithOnePeer thread...");
	int iResult;
	auto pair = *(std::pair <int, SOCKET>*)(lpParam);
	int nodeId = pair.first;
	printf("\nShut down za nodeId = %d, socket =%d", pair.first, pair.second);
	SOCKET nodeSocket = pair.second;

	Message shutDownMsg;
	shutDownMsg.msgType = ShutDown;
	shutDownMsg.size = 4;

	if ((iResult = sendMessage(nodeSocket, &shutDownMsg, 50, 0, INFINITE_ATTEMPT_NO, true)) != shutDownMsg.size + 4)
	{
		printf("\nSomething went wrong with sending shutdown message to peer");
		switch (iResult)
		{
			case ENETDOWN:
			{
				printf("\n WSAENETDOWN");
				exitSignal = true;
			}
			break;
			case WSAECONNRESET:
			{
				printf("\n WSAECONNRESET"); // ovo se desi kad kliknes na x i ugasis node..
			}
			break;
			case WSAECONNABORTED:
			{
				printf("\n WSAECONNABORTED");
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
	}
	else
	{
		//printf("\nSHUT DOWN JE USPESNO POSLAT NODE-u %d ", nodeId);
	}

	RemoveEndpoint(&nodesEndpoints_ForNodes, &cs_NodesEndp, nodeId);
	RemoveEndpoint(&nodesEndpoints_ForClients, &cs_ClientsEndp, nodeId);
	return 0;
}

// send ping message to node on specified interval
DWORD WINAPI PingOneNode(LPVOID lpParam)
{
	int iResult;
	int nodeId = (int)lpParam;
	//printf("\nPing za nodeId = %d",nodeId);
	SOCKET nodeSocket;

	nodeSocket = GetSocket(&nodeSockets, &cs_NodeSockets, nodeId);
	if (nodeSocket == INVALID_SOCKET)
		return 1;

	iResult = send(nodeSocket, NULL, 0, 0);
	if (iResult== SOCKET_ERROR)
	{
		// client has disconnected
		iResult = WSAGetLastError();
		if (iResult == WSAECONNRESET || iResult == WSAECONNABORTED)
		{
			printf("\n Ping za node %d. WSAECONNRESET ili WSAECONNABORTED -> error: %d", nodeId, iResult);
		}
		else
			printf("\n Ping za node %d. error: %d", nodeId, iResult);

		RemoveEndpoint(&nodesEndpoints_ForNodes, &cs_NodesEndp, nodeId);
		RemoveEndpoint(&nodesEndpoints_ForClients, &cs_ClientsEndp, nodeId);
		RemoveSocket(&nodeSockets, &cs_NodeSockets, nodeId);

		if (shutdown(nodeSocket, SD_SEND) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("shutdown(ping - nodeSocket)"));
		}
		if (closesocket(nodeSocket) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("closesocket(ping - nodeSocket)"));
		}

		// if failed node had assigned transaction coordinator role (it failed in
		// the middle of transaction, before he signaled that transaction
		// is finished), reset transaction variables in order to prevent blocking
		// of future transctions
		if (nodeId == CurrentCoordinatorId && IsTransactionOnLine)
		{
			printf("\n Transaction variables reset.");
			IsTransactionOnLine = false;
			CurrentCoordinatorId = -1;
		}
	}
	return 0;
}

DWORD WINAPI ListenForRequestForAssignmentOfTransactionCoordinatorRole(LPVOID lpParam)
{
	int iResult;
	int nodeId = (int)lpParam;
	SOCKET nodeSocket;
	while (!exitSignal)
	{
		nodeSocket = GetSocket(&nodeSockets, &cs_NodeSockets, nodeId);
		if (nodeSocket == INVALID_SOCKET)
		{
			printf("\n nodeSocket %d == INVALID_SOCKET", nodeId);
			break;
		}

		bool shouldSendReply = true;
		Message msgFromNode;
		msgFromNode.size = 0;
		if ((iResult = receiveMessage(nodeSocket, &msgFromNode, 10, 10, 2, false)) == 0)
		{
			Message formedTransReplyMsg;
			formedTransReplyMsg.size = 0;
			if (msgFromNode.msgType == TransactionCoordinatorRequest)
			{
				printf("\n\nRequest for Transaction Manager role received from node %d", nodeId);
				if (IsTransactionOnLine)
				{
					if (nodeId != CurrentCoordinatorId)
					{
						printf("\n Transaction coordinated by other node is already OnLine");
						// error, transaction already in process
						formedTransReplyMsg.size = sizeof(MsgType) + sizeof(Errors);
						formedTransReplyMsg.msgType = TransactionCoordinatorRequestRejected;
						formedTransReplyMsg.payload = (char*)&trOnLineError;
					}
					else
					{
						printf("\n error: TransactionRequest non supported in current context. Node is already coordinator");
						formedTransReplyMsg.size = sizeof(MsgType) + sizeof(Errors);
						formedTransReplyMsg.msgType = Error;
						formedTransReplyMsg.payload = (char*)&nonSupportedError;
					}
				}
				else
				{
					printf("\n TransactionCoordinator role is assigned to node %d", nodeId);
					IsTransactionOnLine = true;
					CurrentCoordinatorId = nodeId;

					formedTransReplyMsg.size = sizeof(MsgType) + 4;
					formedTransReplyMsg.msgType = TransactionCoordinatorRequestApproved;
					formedTransReplyMsg.payload = (char*)&nodeId;
				}
			}
			else if (msgFromNode.msgType == TransactionFinishedRequest)
			{
				if (IsTransactionOnLine)
				{
					shouldSendReply = false;
					printf("\n Request for finishing transaction is received");
					IsTransactionOnLine = false;
					CurrentCoordinatorId = -1;
				}
			}
			else
			{
				printf("\n error: TransactionRequest non supported");
				formedTransReplyMsg.size = sizeof(MsgType) + sizeof(Errors);
				formedTransReplyMsg.msgType = Error;
				formedTransReplyMsg.payload = (char*)&nonSupportedError;
			}

			if (shouldSendReply)
			{
				if ((iResult = sendMessage(nodeSocket, &formedTransReplyMsg, 100, 2, 10, false)) != formedTransReplyMsg.size + 4)
				{
					switch (iResult)
					{
						case WSAENOTSOCK:
						{
							// nor. ako posaljes shutdown koji pogasi sockete...dok je ovaj u send-u
						}
						case ENETDOWN:
						{
							printf("\nsending trans request reply to node  WSAENETDOWN");
							exitSignal = true;
						}
						break;
						case WSAECONNABORTED:
						{
							printf("\n sending trans request reply to node WSAECONNABORTED");
						}
						break;
						case WSAECONNRESET:
						{
							printf("\n sending trans request reply to node WSAECONNRESET");
						}
						break;
						case TIMED_OUT:
						{
							//printf("\n TIMED_OUT");
						}
						break;
						case CLOSED_GRACEFULLY:
						{
							printf("\n sending trans request reply to node CLOSED_GRACEFULLY");
						}
						break;
						default:
						{
							ErrorHandlerTxt(TEXT("Sending trans request reply"));
						}
						break;
					}
				}
			}
		}
		else
		{
			switch (iResult)
			{
				case WSAENOTSOCK:
				{
					// nor. ako posaljes shutdown koji pogasi sockete...dok je ovaj u receiv-u
				}
				case ENETDOWN:
				{
					//printf("\n transaction receiving WSAENETDOWN od node-a %d", nodeId);
					exitSignal = true;
				}
				break;
				case WSAECONNABORTED:
				{
					printf("\n transaction receiving WSAECONNABORTEDod node-a %d", nodeId);
				}
				break;
				case WSAECONNRESET:
				{
					printf("\n transaction receiving WSAECONNRESET od node-a %d", nodeId);
				}
				break;
				case CLOSED_GRACEFULLY:
				{
					printf("\n transaction receiving CLOSED_GRACEFULLY od node-a %d", nodeId);
				}
				break;
				case TIMED_OUT:
				{
					//printf("\n transaction TIMED_OUT od node-a %d", nodeId);
				}
				break;
				case WSAEWOULDBLOCK:
				{
					printf("\n transaction receiving WSAEWOULDBLOCK od node-a %d", nodeId);
				}
				default:
				{
					ErrorHandlerTxt(TEXT("tr other error od node-a "));
				}
				break;
			}
		}

		// if anything is allocated
		if (msgFromNode.size > 4)
			free(msgFromNode.payload);

		Sleep(1000);
	}

	return 0;
}

#pragma endregion

#pragma endregion

#pragma region TimerFunctions

//! very expensive to create and destroy threads constantly...
//todo: consider different approach...
void CALLBACK HealthCheck(UINT timerID, UINT msg, DWORD dwUser, DWORD dw1, DWORD dw2)
{
	HANDLE hThrNodes[MAX_NODES_COUNT];
	DWORD dwThrNodes[MAX_NODES_COUNT];

	EnterCriticalSection(&cs_NodeSockets);
	int nodesCount = nodeSockets.size();

	int i = 0;
	for (auto it = nodeSockets.begin(); it != nodeSockets.end(); ++it)
	{
		hThrNodes[i] = CreateThread(NULL, 0, PingOneNode, (LPVOID)it->first, 0, &dwThrNodes[i]);
		i++;
	}
	LeaveCriticalSection(&cs_NodeSockets);

	WaitForMultipleObjects(nodesCount, hThrNodes, TRUE, INFINITE);

	for (int i = 0; i < nodesCount; i++)
	{
		SAFE_DELETE_HANDLE(hThrNodes[i])
	}

	return;
}

#pragma endregion


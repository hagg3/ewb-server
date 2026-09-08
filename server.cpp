#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <map>
#include <sstream>

#pragma comment(lib, "Ws2_32.lib")

constexpr int DEFAULT_PORT = 27015;
constexpr int BUFFER_SIZE = 512;

// Player info structure
struct PlayerInfo {
	SOCKET socket;
	std::string username;
	int characterType;
	float posX = 0, posY = 0, posZ = 0;
	float velX = 0, velY = 0, velZ = 0;
};

std::vector<SOCKET> clients;
std::map<SOCKET, PlayerInfo> playerInfoMap;
std::mutex clientsMutex;
bool serverRunning = true;

void broadcastMessage(const std::string& message, SOCKET senderSocket) {
	std::lock_guard<std::mutex> lock(clientsMutex);
	for (SOCKET client : clients) {
		if (client != senderSocket) {
			send(client, message.c_str(), static_cast<int>(message.length()), 0);
		}
	}
}

void removeClient(SOCKET clientSocket) {
	std::lock_guard<std::mutex> lock(clientsMutex);
	clients.erase(std::remove(clients.begin(), clients.end(), clientSocket), clients.end());
	playerInfoMap.erase(clientSocket);
}

// Parse message with format "PREFIX:data1:data2:..."
std::vector<std::string> parseMessage(const std::string& message) {
	std::vector<std::string> parts;
	std::stringstream ss(message);
	std::string part;
	while (std::getline(ss, part, ':')) {
		parts.push_back(part);
	}
	return parts;
}

void handleClient(SOCKET clientSocket, int clientId) {
	char recvBuffer[BUFFER_SIZE];
	std::string username = "Player" + std::to_string(clientId);
	int characterType = 0;
	bool initialized = false;

	while (serverRunning) {
		int bytesReceived = recv(clientSocket, recvBuffer, BUFFER_SIZE - 1, 0);

		if (bytesReceived > 0) {
			recvBuffer[bytesReceived] = '\0';
			std::string message(recvBuffer);

			// Parse the message
			auto parts = parseMessage(message);
			if (parts.empty()) continue;

			std::string command = parts[0];

			// Handle JOIN message: "JOIN:username:characterType"
			if (command == "JOIN" && parts.size() >= 3) {
				username = parts[1];
				try {
					characterType = std::stoi(parts[2]);
					if (characterType < 0 || characterType > 10) characterType = 0;
				}
				catch (...) {
					characterType = 0;
				}

				// Store player info
				{
					std::lock_guard<std::mutex> lock(clientsMutex);
					playerInfoMap[clientSocket] = { clientSocket, username, characterType };
				}

				// Send welcome message
				std::string welcome = "[Server] Welcome, " + username + "! (Character Type: " + std::to_string(characterType) + ")\n";
				send(clientSocket, welcome.c_str(), static_cast<int>(welcome.length()), 0);

				// Notify others
				std::string joinMsg = "[Server] " + username + " (Type " + std::to_string(characterType) + ") has joined.\n";
				std::cout << joinMsg;
				broadcastMessage(joinMsg, clientSocket);
				initialized = true;
			}
			// Handle MSG message: "MSG:actual message content"
			else if (command == "MSG" && parts.size() >= 2) {
				// Reconstruct message content (in case it contained colons)
				std::string msgContent = message.substr(4); // Skip "MSG:"

				// Check for exit command
				if (msgContent == "exit" || msgContent == "quit") {
					std::cout << "[Server] " << username << " disconnected." << std::endl;
					break;
				}

				// Format and broadcast message
				std::string broadcastMsg = "[" + username + " (T" + std::to_string(characterType) + ")] " + msgContent + "\n";
				std::cout << broadcastMsg;
				broadcastMessage(broadcastMsg, clientSocket);
			}
			// Handle ACTION message: "ACTION:x:y:z:mode[:typeOrColor]"
			// mode: 0=build, 1=mine, 2=burn, 3=paint
			else if (command == "ACTION" && parts.size() >= 5) {
				try {
					int x = std::stoi(parts[1]);
					int y = std::stoi(parts[2]);
					int z = std::stoi(parts[3]);
					int mode = std::stoi(parts[4]);

					std::string actionStr;
					std::string broadcastMsg;

					switch (mode) {
						case 0: { // BUILD
							int blockType = (parts.size() >= 6) ? std::stoi(parts[5]) : 0;
							actionStr = "BUILD";
							broadcastMsg = "ACTION:" + username + ":" + std::to_string(characterType) + ":" +
										   std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z) + ":" +
										   std::to_string(mode) + ":" + std::to_string(blockType) + "\n";
							std::cout << "[" << username << "] BUILD at (" << x << "," << y << "," << z << ") type=" << blockType << std::endl;
							break;
						}
						case 1: { // MINE
							actionStr = "MINE";
							broadcastMsg = "ACTION:" + username + ":" + std::to_string(characterType) + ":" +
										   std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z) + ":" +
										   std::to_string(mode) + "\n";
							std::cout << "[" << username << "] MINE at (" << x << "," << y << "," << z << ")" << std::endl;
							break;
						}
						case 2: { // BURN
							actionStr = "BURN";
							broadcastMsg = "ACTION:" + username + ":" + std::to_string(characterType) + ":" +
										   std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z) + ":" +
										   std::to_string(mode) + "\n";
							std::cout << "[" << username << "] BURN at (" << x << "," << y << "," << z << ")" << std::endl;
							break;
						}
						case 3: { // PAINT
							int color = (parts.size() >= 6) ? std::stoi(parts[5]) : 0;
							actionStr = "PAINT";
							broadcastMsg = "ACTION:" + username + ":" + std::to_string(characterType) + ":" +
										   std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z) + ":" +
										   std::to_string(mode) + ":" + std::to_string(color) + "\n";
							std::cout << "[" << username << "] PAINT at (" << x << "," << y << "," << z << ") color=" << color << std::endl;
							break;
						}
											default:
												std::cout << "[" << username << "] Unknown action mode: " << mode << std::endl;
												continue;
										}

										// Broadcast to all clients (including sender for confirmation)
										broadcastMessage(broadcastMsg, INVALID_SOCKET);
									}
									catch (...) {
										std::cout << "[Server] Invalid ACTION message from " << username << std::endl;
									}
								}
								// Handle POS message: "POS:x:y:z"
								else if (command == "POS" && parts.size() >= 4) {
									try {
										float px = std::stof(parts[1]);
										float py = std::stof(parts[2]);
										float pz = std::stof(parts[3]);

										// Update player info
										{
											std::lock_guard<std::mutex> lock(clientsMutex);
											if (playerInfoMap.count(clientSocket)) {
												playerInfoMap[clientSocket].posX = px;
												playerInfoMap[clientSocket].posY = py;
												playerInfoMap[clientSocket].posZ = pz;
											}
										}

										std::cout << "[" << username << "] POS (" << px << "," << py << "," << pz << ")" << std::endl;

										// Broadcast position to all other clients
										std::string broadcastMsg = "POS:" + username + ":" + std::to_string(characterType) + ":" +
																   parts[1] + ":" + parts[2] + ":" + parts[3] + "\n";
										broadcastMessage(broadcastMsg, clientSocket);
									}
									catch (...) {
										std::cout << "[Server] Invalid POS message from " << username << std::endl;
									}
								}
								// Handle VEL message: "VEL:x:y:z"
								else if (command == "VEL" && parts.size() >= 4) {
									try {
										float vx = std::stof(parts[1]);
										float vy = std::stof(parts[2]);
										float vz = std::stof(parts[3]);

										// Update player info
										{
											std::lock_guard<std::mutex> lock(clientsMutex);
											if (playerInfoMap.count(clientSocket)) {
												playerInfoMap[clientSocket].velX = vx;
												playerInfoMap[clientSocket].velY = vy;
												playerInfoMap[clientSocket].velZ = vz;
											}
										}

										std::cout << "[" << username << "] VEL (" << vx << "," << vy << "," << vz << ")" << std::endl;

										// Broadcast velocity to all other clients
										std::string broadcastMsg = "VEL:" + username + ":" + std::to_string(characterType) + ":" +
																   parts[1] + ":" + parts[2] + ":" + parts[3] + "\n";
										broadcastMessage(broadcastMsg, clientSocket);
									}
									catch (...) {
										std::cout << "[Server] Invalid VEL message from " << username << std::endl;
									}
								}
								// Handle POSVEL message: "POSVEL:px:py:pz:vx:vy:vz"
								else if (command == "POSVEL" && parts.size() >= 7) {
									try {
										float px = std::stof(parts[1]);
										float py = std::stof(parts[2]);
										float pz = std::stof(parts[3]);
										float vx = std::stof(parts[4]);
										float vy = std::stof(parts[5]);
										float vz = std::stof(parts[6]);

										// Update player info
										{
											std::lock_guard<std::mutex> lock(clientsMutex);
											if (playerInfoMap.count(clientSocket)) {
												auto& info = playerInfoMap[clientSocket];
												info.posX = px; info.posY = py; info.posZ = pz;
												info.velX = vx; info.velY = vy; info.velZ = vz;
											}
										}

										std::cout << "[" << username << "] POSVEL pos=(" << px << "," << py << "," << pz << ") "
												  << "vel=(" << vx << "," << vy << "," << vz << ")" << std::endl;

										// Broadcast to all other clients
										std::string broadcastMsg = "POSVEL:" + username + ":" + std::to_string(characterType) + ":" +
																   parts[1] + ":" + parts[2] + ":" + parts[3] + ":" +
																   parts[4] + ":" + parts[5] + ":" + parts[6] + "\n";
										broadcastMessage(broadcastMsg, clientSocket);
									}
									catch (...) {
										std::cout << "[Server] Invalid POSVEL message from " << username << std::endl;
									}
								}
							}
							else {
								// Client disconnected or error
								std::cout << "[Server] " << username << " disconnected." << std::endl;
								break;
							}
						}

						// Notify others of disconnect
						std::string leaveMsg = "[Server] " + username + " has left.\n";
						broadcastMessage(leaveMsg, clientSocket);

						// Cleanup
						removeClient(clientSocket);
						closesocket(clientSocket);
						}

int main() {
	WSADATA wsaData;
	SOCKET listenSocket = INVALID_SOCKET;

	// Initialize Winsock
	int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (result != 0) {
		std::cerr << "WSAStartup failed: " << result << std::endl;
		return 1;
	}

	// Create socket address info
	sockaddr_in serverAddr{};
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	serverAddr.sin_port = htons(DEFAULT_PORT);

	// Create listening socket
	listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSocket == INVALID_SOCKET) {
		std::cerr << "Socket creation failed: " << WSAGetLastError() << std::endl;
		WSACleanup();
		return 1;
	}

	// Allow socket reuse
	int opt = 1;
	setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&opt), sizeof(opt));

	// Bind socket
	result = bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
	if (result == SOCKET_ERROR) {
		std::cerr << "Bind failed: " << WSAGetLastError() << std::endl;
		closesocket(listenSocket);
		WSACleanup();
		return 1;
	}

	// Listen for connections
	result = listen(listenSocket, SOMAXCONN);
	if (result == SOCKET_ERROR) {
		std::cerr << "Listen failed: " << WSAGetLastError() << std::endl;
		closesocket(listenSocket);
		WSACleanup();
		return 1;
	}

	std::cout << "========================================" << std::endl;
	std::cout << "  TCP/IP Chat Server" << std::endl;
	std::cout << "  Listening on port " << DEFAULT_PORT << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << "Waiting for clients to connect..." << std::endl;

	int clientIdCounter = 0;
	std::vector<std::thread> clientThreads;

	while (serverRunning) {
		// Accept client connection
		sockaddr_in clientAddr{};
		int clientAddrLen = sizeof(clientAddr);
		SOCKET clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);

		if (clientSocket == INVALID_SOCKET) {
			if (serverRunning) {
				std::cerr << "Accept failed: " << WSAGetLastError() << std::endl;
			}
			continue;
		}

		// Get client IP address
		char clientIP[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);

		int clientId = ++clientIdCounter;
		std::cout << "[Server] Client #" << clientId << " connected from " << clientIP << std::endl;

		// Add to clients list
		{
			std::lock_guard<std::mutex> lock(clientsMutex);
			clients.push_back(clientSocket);
		}

		// Start client handler thread
		clientThreads.emplace_back(handleClient, clientSocket, clientId);
	}

	// Cleanup
	closesocket(listenSocket);
	WSACleanup();

	std::cout << "Server terminated." << std::endl;
	return 0;
}

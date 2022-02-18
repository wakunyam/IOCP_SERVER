#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <windows.h>
#include <iostream>
#include <unordered_map>
#include <chrono>
using namespace std;
using namespace chrono;

#include "../../2020_IOCP_Server/2020_IOCP_Server/protocol.h"

sf::TcpSocket g_socket;

constexpr auto SCREEN_WIDTH = 20;
constexpr auto SCREEN_HEIGHT = 20;

constexpr int chat_log_num = 10;
constexpr auto TILE_WIDTH = 32;
constexpr auto WINDOW_WIDTH = TILE_WIDTH * SCREEN_WIDTH + 150;   // size of window
constexpr auto WINDOW_HEIGHT = TILE_WIDTH * SCREEN_WIDTH + 50 + chat_log_num * 20;
constexpr auto BUF_SIZE = 200;

// 추후 확장용.
int NPC_ID_START = 10000;

int g_left_x;
int g_top_y;
int g_myid;

struct USER_DATA {
	int hp;
	char level;
	int exp;
};

USER_DATA my_data;

sf::RenderWindow* g_window;
sf::Font g_font;

class OBJECT {
private:
	bool m_showing;

	char m_mess[MAX_STR_LEN];
	high_resolution_clock::time_point m_time_out;
	sf::Text m_text;
	sf::Text m_name;
public:
	sf::Sprite m_sprite;
	int m_x, m_y;
	char name[MAX_ID_LEN];
	OBJECT(sf::Texture& t, int x, int y, int x2, int y2) {
		m_showing = false;
		m_sprite.setTexture(t);
		m_sprite.setTextureRect(sf::IntRect(x, y, x2, y2));
		m_time_out = high_resolution_clock::now();
	}
	OBJECT() {
		m_showing = false;
		m_time_out = high_resolution_clock::now();
	}
	void show()
	{
		m_showing = true;
	}
	void hide()
	{
		m_showing = false;
	}

	void a_move(int x, int y) {
		m_sprite.setPosition((float)x, (float)y);
	}

	void a_draw() {
		g_window->draw(m_sprite);
	}

	void move(int x, int y) {
		m_x = x;
		m_y = y;
	}
	void draw() {
		if (false == m_showing) return;
		float rx = (m_x - g_left_x) * TILE_WIDTH + 8;
		float ry = (m_y - g_top_y) * TILE_WIDTH + 8;
		m_sprite.setPosition(rx, ry);
		g_window->draw(m_sprite);
		m_name.setPosition(rx - 10, ry - 10);
		g_window->draw(m_name);
		if (high_resolution_clock::now() < m_time_out) {
			m_text.setPosition(rx - 10, ry + 20);
			g_window->draw(m_text);
		}
	}
	void set_name(char str[]) {
		m_name.setFont(g_font);
		m_name.setString(str);
		m_name.setFillColor(sf::Color(255, 255, 0));
		m_name.setStyle(sf::Text::Bold);
		m_name.setScale(0.5f, 0.5f);
	}
	void add_chat(char chat[]) {
		m_text.setFont(g_font);
		m_text.setString(chat);
		m_time_out = high_resolution_clock::now() + 1s;
	}
};

OBJECT avatar;
unordered_map <int, OBJECT> npcs;

OBJECT white_tile;
OBJECT black_tile;
OBJECT tree_tile;

sf::Texture* board;
sf::Texture* pieces;
sf::Texture* tree;

char chat_message[MAX_STR_LEN];
char chat_log[chat_log_num][MAX_STR_LEN];
int chat_len = 0;

void client_initialize()
{
	board = new sf::Texture;
	pieces = new sf::Texture;
	tree = new sf::Texture;
	if (false == g_font.loadFromFile("cour.ttf")) {
		cout << "Font Loading Error!\n";
		while (true);
	}
	board->loadFromFile("chessmap.bmp");
	pieces->loadFromFile("chess2.png");
	tree->loadFromFile("tree.png");
	white_tile = OBJECT{ *board, 5, 5, 64, 64 };
	white_tile.m_sprite.setScale(0.5f, 0.5f);
	black_tile = OBJECT{ *board, 69, 5, 64, 64 };
	black_tile.m_sprite.setScale(0.5f, 0.5f);
	tree_tile = OBJECT{ *tree, 0, 0, 64, 64 };
	tree_tile.m_sprite.setScale(0.5f, 0.5f);
	avatar = OBJECT{ *pieces, 128, 0, 64, 64 };
	avatar.m_sprite.setScale(0.5f, 0.5f);
	avatar.move(4, 4);
}

void client_finish()
{
	delete board;
	delete pieces;
}

void ProcessPacket(char* ptr)
{
	static bool first_time = true;
	switch (ptr[1]) {
	case SC_PACKET_LOGIN_OK: {
		sc_packet_login_ok* my_packet = reinterpret_cast<sc_packet_login_ok*>(ptr);
		g_myid = my_packet->id;
		avatar.move(my_packet->x, my_packet->y);
		g_left_x = my_packet->x - (SCREEN_WIDTH / 2);
		g_top_y = my_packet->y - (SCREEN_HEIGHT / 2);
		avatar.show();
		my_data.exp = my_packet->exp;
		my_data.hp = my_packet->hp;
		my_data.level = my_packet->level;
		break;
	}
	case SC_PACKET_LOGIN_FAIL: {
		sc_packet_login_fail* p = reinterpret_cast<sc_packet_login_fail*>(ptr);
		cout << p->message;
		g_window->close();
		break;
	}
	case SC_PACKET_ENTER: {
		sc_packet_enter* my_packet = reinterpret_cast<sc_packet_enter*>(ptr);
		int id = my_packet->id;

		if (id == g_myid) {
			avatar.move(my_packet->x, my_packet->y);
			g_left_x = my_packet->x - (SCREEN_WIDTH / 2);
			g_top_y = my_packet->y - (SCREEN_HEIGHT / 2);
			avatar.show();
		}
		else {
			if (id < NPC_ID_START) {
				npcs[id] = OBJECT{ *pieces, 64, 0, 64, 64 };
				npcs[id].m_sprite.setScale(0.5f, 0.5f);
			}
			else {
				switch (my_packet->o_type) {
				case 0:
					npcs[id] = OBJECT{ *pieces, 256, 0, 64, 64 };
					break;
				case 1:
					npcs[id] = OBJECT{ *pieces, 192, 0, 64, 64 };
					break;
				case 2:
					npcs[id] = OBJECT{ *pieces, 320, 0, 64, 64 };
					break;
				}
				npcs[id].m_sprite.setScale(0.5f, 0.5f);
			}
			strcpy_s(npcs[id].name, my_packet->name);
			npcs[id].set_name(my_packet->name);
			npcs[id].move(my_packet->x, my_packet->y);
			npcs[id].show();
		}
		break;
	}
	case SC_PACKET_MOVE: {
		sc_packet_move* my_packet = reinterpret_cast<sc_packet_move*>(ptr);
		int other_id = my_packet->id;
		if (other_id == g_myid) {
			avatar.move(my_packet->x, my_packet->y);
			g_left_x = my_packet->x - (SCREEN_WIDTH / 2);
			g_top_y = my_packet->y - (SCREEN_HEIGHT / 2);
		}
		else {
			if (0 != npcs.count(other_id))
				npcs[other_id].move(my_packet->x, my_packet->y);
		}
		break;
	}
	case SC_PACKET_LEAVE: {
		sc_packet_leave* my_packet = reinterpret_cast<sc_packet_leave*>(ptr);
		int other_id = my_packet->id;
		if (other_id == g_myid) {
			avatar.hide();
		}
		else {
			if (0 != npcs.count(other_id))
				npcs[other_id].hide();
		}
		break;
	}
	case SC_PACKET_CHAT: {
		sc_packet_chat* my_packet = reinterpret_cast<sc_packet_chat*>(ptr);
		int size = my_packet->size;
		my_packet->message[size - 6] = '\0';
		if (g_myid != my_packet->id) {
			npcs[my_packet->id].add_chat(my_packet->message);
		}
		else {
			avatar.add_chat(my_packet->message);
		}
		if (my_packet->id < MAX_USER) {
			for (int i = chat_log_num - 2; i >= 0; --i) {
				strncpy_s(chat_log[i + 1], chat_log[i], strlen(chat_log[i]));
			}
			strncpy_s(chat_log[0], my_packet->message, strlen(my_packet->message));
		}
		break;
	}
	case SC_PACKET_STAT_CHANGE: {
		sc_packet_stat_change* p = reinterpret_cast<sc_packet_stat_change*>(ptr);
		int id = p->id;

		if (id == g_myid) {
			if (my_data.level != p->level) {
				for (int i = chat_log_num - 2; i >= 0; --i) {
					strncpy_s(chat_log[i + 1], chat_log[i], strlen(chat_log[i]));
				}
				sprintf_s(chat_log[0], "Your level has risen to %d.", p->level);		
			}
			else if (my_data.exp != p->exp) {
				for (int i = chat_log_num - 2; i >= 0; --i) {
					strncpy_s(chat_log[i + 1], chat_log[i], strlen(chat_log[i]));
				}
				sprintf_s(chat_log[0], "You gained %d experience by defeating monsters.", p->exp - my_data.exp);		
			}
			if (my_data.hp != p->hp) {
				for (int i = chat_log_num - 2; i >= 0; --i) {
					strncpy_s(chat_log[i + 1], chat_log[i], strlen(chat_log[i]));
				}
				if (p->hp >= my_data.hp) {
					sprintf_s(chat_log[0], "You have recovered %dhp.", p->hp - my_data.hp);
				}
				else
					sprintf_s(chat_log[0], "You was damaged %d from monster's attack.", my_data.hp - p->hp);
			}
			my_data.level = p->level;
			my_data.exp = p->exp;
			my_data.hp = p->hp;
		}
		else {
			for (int i = chat_log_num - 2; i >= 0; --i) {
				strncpy_s(chat_log[i + 1], chat_log[i], strlen(chat_log[i]));
			}
			sprintf_s(chat_log[0], "You deals %d damage to a monster.", p->exp);
		}
		break;
	}
	default:
		printf("Unknown PACKET type [%d]\n", ptr[1]);
	}
}

void process_data(char* net_buf, size_t io_byte)
{
	char* ptr = net_buf;
	static size_t in_packet_size = 0;
	static size_t saved_packet_size = 0;
	static char packet_buffer[BUF_SIZE];

	while (0 != io_byte) {
		if (0 == in_packet_size) in_packet_size = ptr[0];
		if (io_byte + saved_packet_size >= in_packet_size) {
			memcpy(packet_buffer + saved_packet_size, ptr, in_packet_size - saved_packet_size);
			ProcessPacket(packet_buffer);
			ptr += in_packet_size - saved_packet_size;
			io_byte -= in_packet_size - saved_packet_size;
			in_packet_size = 0;
			saved_packet_size = 0;
		}
		else {
			memcpy(packet_buffer + saved_packet_size, ptr, io_byte);
			saved_packet_size += io_byte;
			io_byte = 0;
		}
	}
}

void client_main()
{
	char net_buf[BUF_SIZE];
	size_t	received;

	auto recv_result = g_socket.receive(net_buf, BUF_SIZE, received);
	if (recv_result == sf::Socket::Error)
	{
		wcout << L"Recv 에러!";
		while (true);
	}

	if (recv_result == sf::Socket::Disconnected)
	{
		wcout << L"서버 접속 종료.";
		g_window->close();
	}

	if (recv_result != sf::Socket::NotReady)
		if (received > 0) process_data(net_buf, received);

	for (int i = 0; i < SCREEN_WIDTH; ++i)
		for (int j = 0; j < SCREEN_HEIGHT; ++j)
		{
			int tile_x = i + g_left_x;
			int tile_y = j + g_top_y;
			if ((tile_x < 0) || (tile_y < 0)) continue;
			//if (((tile_x + tile_y) % 2) == 0) {
			if (((tile_x / 3 + tile_y / 3) % 2) == 0) {
				white_tile.a_move(TILE_WIDTH * i + 7, TILE_WIDTH * j + 7);
				white_tile.a_draw();
			}
			else {
				black_tile.a_move(TILE_WIDTH * i + 7, TILE_WIDTH * j + 7);
				black_tile.a_draw();
			}
			if ((tile_x % 8 == 0) && (tile_y % 8 == 0)) {
				tree_tile.a_move(TILE_WIDTH * i + 7, TILE_WIDTH * j + 7);
				tree_tile.a_draw();
			}
		}
	avatar.draw();
	//	for (auto &pl : players) pl.draw();
	for (auto& npc : npcs) npc.second.draw();
	sf::Text text;
	text.setFont(g_font);
	char buf[100];
	sprintf_s(buf, "(%d, %d)", avatar.m_x, avatar.m_y);
	text.setString(buf);
	g_window->draw(text);

	sf::Text chatText;
	chatText.setFont(g_font);
	chatText.setString(chat_message);
	chatText.setPosition(0, 850);
	g_window->draw(chatText);

	sf::Text chatLog[chat_log_num];
	for (int i = 0; i < chat_log_num; ++i) {
		chatLog[i].setFont(g_font);
		chatLog[i].setString(chat_log[i]);
		chatLog[i].setPosition(0, 850 - 20 * (i + 1));
		chatLog[i].setCharacterSize(20);
		g_window->draw(chatLog[i]);
	}

	sf::Text hp;
	hp.setFont(g_font);
	char hbuf[25];
	sprintf_s(hbuf, "Hp:%d", my_data.hp);
	hp.setString(hbuf);
	hp.setPosition(650, 0);
	hp.setCharacterSize(20);
	g_window->draw(hp);

	sf::Text level;
	level.setFont(g_font);
	char lbuf[25];
	sprintf_s(lbuf, "Level:%d", my_data.level);
	level.setString(lbuf);
	level.setPosition(650, 20);
	level.setCharacterSize(20);
	g_window->draw(level);

	sf::Text exp;
	exp.setFont(g_font);
	char ebuf[25];
	sprintf_s(ebuf, "Exp:%d", my_data.exp);
	exp.setString(ebuf);
	exp.setPosition(650, 40);
	exp.setCharacterSize(20);
	g_window->draw(exp);
}

void send_packet(void* packet)
{
	char* p = reinterpret_cast<char*>(packet);
	size_t sent;
	g_socket.send(p, p[0], sent);
}

void send_move_packet(unsigned char dir)
{
	cs_packet_move m_packet;
	m_packet.type = CS_MOVE;
	m_packet.size = sizeof(m_packet);
	m_packet.direction = dir;
	send_packet(&m_packet);
}

void send_chat_packet(char* mes, size_t size)
{
	cs_packet_chat packet;
	strncpy_s(packet.message, mes, size);
	packet.size = sizeof(packet) - 100 + size;
	packet.type = CS_CHAT;
	send_packet(&packet);
}

void send_attack_packet()
{
	cs_packet_attack packet;
	packet.size = sizeof(packet);
	packet.type = CS_ATTACK;
	send_packet(&packet);
}

void send_logout_packet()
{
	cs_packet_logout packet;
	packet.size = sizeof(packet);
	packet.type = CS_LOGOUT;
	send_packet(&packet);
}

void send_teleport_packet()
{
	cs_packet_teleport packet;
	packet.size = sizeof(packet);
	packet.type = CS_TELEORT;
}

int main()
{
	wcout.imbue(locale("korean"));
	sf::Socket::Status status = g_socket.connect("127.0.0.1", SERVER_PORT);
	g_socket.setBlocking(false);

	if (status != sf::Socket::Done) {
		wcout << L"서버와 연결할 수 없습니다.\n";
		while (true);
	}

	client_initialize();

	cs_packet_login l_packet;
	l_packet.size = sizeof(l_packet);
	l_packet.type = CS_LOGIN;
	cout << "ID를 입력하세요.(최대 9자) : ";
	char name[MAX_ID_LEN];
	cin >> name;
	strcpy_s(l_packet.name, name);

	/*int t_id = GetCurrentProcessId();
	sprintf_s(l_packet.name, "P%03d", t_id % 1000);*/
	strcpy_s(avatar.name, l_packet.name);
	avatar.set_name(l_packet.name);
	send_packet(&l_packet);

	char net_buf[BUF_SIZE];
	size_t	received;

	auto recv_result = g_socket.receive(net_buf, BUF_SIZE, received);
	if (recv_result == sf::Socket::Error)
	{
		wcout << L"Recv 에러!";
		while (true);
	}

	if (recv_result == sf::Socket::Disconnected)
	{
		wcout << L"서버 접속 종료.";
		g_window->close();
	}

	if (recv_result != sf::Socket::NotReady)
		if (received > 0) process_data(net_buf, received);

	sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "2D CLIENT");
	g_window = &window;

	bool chat_mode = false;

	while (window.isOpen())
	{
		sf::Event event;
		while (window.pollEvent(event))
		{
			if (event.type == sf::Event::Closed)
				window.close();
			if (event.type == sf::Event::KeyPressed) {
				int p_type = -1;
				if (false == chat_mode) {
					switch (event.key.code) {
					case sf::Keyboard::Left:
						send_move_packet(MV_LEFT);
						break;
					case sf::Keyboard::Right:
						send_move_packet(MV_RIGHT);
						break;
					case sf::Keyboard::Up:
						send_move_packet(MV_UP);
						break;
					case sf::Keyboard::Down:
						send_move_packet(MV_DOWN);
						break;
					case sf::Keyboard::A:
						send_attack_packet();
						break;
					case sf::Keyboard::Enter:
						chat_mode = true;
						break;
					case sf::Keyboard::Escape:
						window.close();
						break;
					}
				}
				else {
					if (sf::Keyboard::A <= event.key.code && event.key.code <= sf::Keyboard::Z) {
						if (chat_len < 98)
							chat_message[chat_len++] = event.key.code + 65;
					}
					else {
						switch (event.key.code) {
						case sf::Keyboard::BackSpace:
							chat_message[chat_len] = '\0';
							if (chat_len > 0) --chat_len;
							break;
						case sf::Keyboard::Space:
							chat_message[chat_len] = ' ';
							if (chat_len < 98) ++chat_len;
							break;
						case sf::Keyboard::Enter:
							chat_mode = false;
							if (chat_len < 1) break;
							send_chat_packet(chat_message, chat_len);
							chat_len = 0;
							for (int i = 0; i < 100; ++i) chat_message[i] = '\0';
							break;
						}
					}
				}
			}
		}

		window.clear();
		client_main();
		window.display();
	}
	send_logout_packet();
	client_finish();

	return 0;
}
#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <chrono>
#include <queue>
#include <atomic>
#include <Windows.h>
#include <sqlext.h>
#include <comdef.h>
#include <tbb/concurrent_priority_queue.h>
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_hash_map.h>
#include <shared_mutex>
#include <tbb/parallel_for.h>

extern "C" {
#include "include/lua.h"
#include "include/lauxlib.h"
#include "include/lualib.h"
}

#include "protocol.h"
using namespace std;
using namespace chrono;
using namespace tbb;
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "lua54.lib")
#pragma comment (lib, "odbc32.lib")
constexpr int MAX_BUFFER = 4096;

constexpr char OP_MODE_RECV = 0;
constexpr char OP_MODE_SEND = 1;
constexpr char OP_MODE_ACCEPT = 2;
constexpr char OP_RANDOM_MOVE = 3;
constexpr char OP_GET_USERDATA = 4;
constexpr char OP_SAVE_USERDATA = 5;
constexpr char OP_ATTACK_DELAY = 6;
constexpr char OP_AUTO_RECOVERY = 7;
constexpr char OP_RESURRECTION = 8;
constexpr char OP_NPC_ACT = 9;

constexpr int HEALTH_PER_LEVEL = 100;
constexpr int LEVEL_ONE_EXP = 100;
constexpr int REQ_EXP_MUL = 2;

constexpr int KEY_SERVER = 1000000;

constexpr int SECTOR_WIDTH = 20;
constexpr int SECTOR_HEIGHT = 20;

struct USER_DATA {
	char name[MAX_ID_LEN];
	char level;
	int hp, exp;
	short x, y;
};

struct T_OVER_EX {
	WSAOVERLAPPED wsa_over;
	char op_mode;
};

struct DB_OVER_EX {
	WSAOVERLAPPED wsa_over;
	char op_mode;
	int object_id;
	char name[10];
};

struct OVER_EX {
	WSAOVERLAPPED wsa_over;
	char op_mode;
	WSABUF wsa_buf;
	unsigned char iocp_buf[MAX_BUFFER];
	int		object_id;
};

class object_info {
public:
	shared_mutex c_lock;
	char name[MAX_ID_LEN];
	short x, y;
	int hp;
	char level;
	short damage = 10;
	bool att_delay;
	volatile bool is_active;
};

class npc_info : public object_info {
public:
	char type;
	bool live;
	mutex lua_l;
	lua_State* L;
};

class client_info : public object_info {
public:
	SOCKET	m_sock;
	OVER_EX	m_recv_over;
	unsigned char* m_packet_start;
	unsigned char* m_recv_start;

	shared_mutex vl;
	unordered_set<int> view_list;

	int move_time;

	int exp;
	volatile bool recovering;
};

class NODE {
public:
	OVER_EX over;
	NODE* volatile next;

	NODE()
	{
		over.wsa_buf.buf = reinterpret_cast<CHAR*>(over.iocp_buf);
		ZeroMemory(&over.wsa_over, sizeof(over.wsa_over));
		next = nullptr;
	}
	~NODE() {}
};

class FREELIST
{
	NODE* head, * tail;
public:
	FREELIST()
	{
		head = tail = new NODE();
	}
	~FREELIST()
	{
		while (nullptr != head) {
			NODE* p = head;
			head = head->next;
			delete p;
		}
	}
	NODE* get_node()
	{
		if (head == tail) return new NODE();
		NODE* p = head;
		head = head->next;
		p->next = nullptr;
		return p;
	}
	void free_node(NODE* p)
	{
		p->next = nullptr;
		ZeroMemory(&p->over.wsa_over, sizeof(p->over.wsa_over));
		tail->next = p;
		tail = p;
		//delete p;
	}
};

class LFFREELIST
{
	NODE* volatile head;
	NODE* volatile tail;
public:
	LFFREELIST()
	{
		head = tail = new NODE();
	}
	~LFFREELIST()
	{
		while (nullptr != head) {
			NODE* p = head;
			head = head->next;
			delete p;
		}
	}
	bool CAS(NODE* volatile* next, NODE* old_p, NODE* new_p)
	{
		return atomic_compare_exchange_strong(
			reinterpret_cast<volatile atomic_int*>(next),
			reinterpret_cast<int*>(&old_p),
			reinterpret_cast<int>(new_p)
		);
	}
	NODE* get_node()
	{
		while (true) {
			NODE* first = head;
			NODE* last = tail;
			NODE* next = first->next;
			if (first != head) continue;
			if (nullptr == next) return new NODE();
			if (first == last) {
				CAS(&tail, last, next);
				continue;
			}
			if (false == CAS(&head, first, next)) continue;
			return first;
		}
	}
	void free_node(NODE* p)
	{
		p->next = nullptr;
		ZeroMemory(&p->over.wsa_over, sizeof(p->over.wsa_over));
		while (true) {
			NODE* last = tail;
			NODE* next = last->next;
			if (last != tail) continue;
			if (nullptr == next) {
				if (CAS(&(last->next), nullptr, p)) {
					CAS(&tail, last, p);
				}
			}
			else CAS(&tail, last, next);
		}
	}
};

struct pair_hash {
	template<class T1, class T2>
	size_t operator()(const pair<T1, T2>& p) const {
		auto h1 = hash<T1>{}(p.first);
		auto h2 = hash<T2>{}(p.second);

		return h1 ^ h2;
	}
};

struct SECTOR {
	unordered_set<int> sector;
	shared_mutex s_lock;
};

struct event_type {
	int obj_id;
	int event_id;
	system_clock::time_point wakeup_time;
	int target_id;

	constexpr bool operator < (const event_type& _Left) const
	{
		return (wakeup_time > _Left.wakeup_time);
	}
	event_type() {}
	event_type(int obj_id, int event_type, system_clock::time_point t)
		:obj_id(obj_id), event_id(event_type), wakeup_time(t) {}
};

struct db_event_type {
	int obj_id;
	int event_id;
	char name[MAX_ID_LEN];

	db_event_type() {}
	db_event_type(int id, int e_id, const char* n) :obj_id{ id }, event_id{ e_id }{
		strcpy_s(name, n);
	}
	db_event_type(int id, int e_id) :obj_id{ id }, event_id{ e_id }{}
};

mutex id_lock;
client_info g_clients[MAX_USER];

npc_info g_npcs[NUM_NPC];
HANDLE h_iocp;
SOCKET g_lSocket;
OVER_EX g_accept_over;

thread_local FREELIST freelist;
//LFFREELIST freelist;
SECTOR sectors[WORLD_HEIGHT / SECTOR_HEIGHT][WORLD_WIDTH / SECTOR_WIDTH];

SQLHENV henv;
SQLHDBC hdbc;
SQLHSTMT hstmt = 0;
SQLRETURN retcode;

priority_queue<event_type> timer_queue;
shared_mutex timer_l;
//concurrent_priority_queue<event_type> timer_queue;

//queue<db_event_type> db_queue;
shared_mutex db_l;
concurrent_queue<db_event_type> db_queue;

void error(lua_State* L, const char* fmt, ...)
{
	va_list argp; va_start(argp, fmt);
	vfprintf(stderr, fmt, argp);
	va_end(argp);
	lua_close(L);
	exit(EXIT_FAILURE);
}

bool CAS(volatile bool* addr, bool old_v, bool new_v)
{
	return atomic_compare_exchange_strong(
		reinterpret_cast<volatile atomic_bool*>(addr),
		&old_v, new_v
	);
}

void show_error(SQLHANDLE hHandle, SQLSMALLINT hType, RETCODE RetCode)
{
	SQLSMALLINT iRec = 0;
	SQLINTEGER iError;
	WCHAR wszMessage[1000];
	WCHAR wszState[SQL_SQLSTATE_SIZE + 1];
	if (RetCode == SQL_INVALID_HANDLE) {
		wcout << L"Invalid handle!\n";
		return;
	}
	while (SQLGetDiagRec(hType, hHandle, ++iRec, wszState, &iError, wszMessage,
		(SQLSMALLINT)(sizeof(wszMessage) / sizeof(WCHAR)), (SQLSMALLINT*)NULL) == SQL_SUCCESS) {
		// Hide data truncated..
		if (wcsncmp(wszState, L"01004", 5)) {
			wcout << L"[" << wszState << "] " << wszMessage << "(" << iError << ")\n";
		}
	}
}

void db_worker()
{
	std::wcout.imbue(std::locale("korean"));

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	// Set the ODBC version environment attribute  
	if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
		retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

		// Allocate connection handle  
		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

			// Set login timeout to 5 seconds  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

				// Connect to data source  
				retcode = SQLConnect(hdbc, (SQLWCHAR*)L"2020_2_GS", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

				// Allocate statement handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					cout << "ODBC connect OK!\n";
					retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
				}
				else {
					show_error(hdbc, SQL_HANDLE_DBC, retcode);
					SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
					SQLFreeHandle(SQL_HANDLE_ENV, henv);
					return;
				}
			}
		}
		else {
			show_error(henv, SQL_HANDLE_ENV, retcode);
			SQLFreeHandle(SQL_HANDLE_ENV, henv);
			return;
		}
	}
	else {
		show_error(henv, SQL_HANDLE_ENV, retcode);
		return;
	}

	while (true) {
		this_thread::sleep_for(10ms);
		while (true) {
			db_event_type ev;
			/*db_l.lock();
			if (true == db_queue.empty()) {
				db_l.unlock();
				break;
			}
			ev = db_queue.front();
			db_queue.pop();
			db_l.unlock();*/
			if (false == db_queue.try_pop(ev)) break;
			switch (ev.event_id)
			{
			case OP_GET_USERDATA: {
				DB_OVER_EX* over = new DB_OVER_EX;
				over->op_mode = ev.event_id;
				over->object_id = ev.obj_id;
				strcpy_s(over->name, ev.name);
				ZeroMemory(&over->wsa_over, sizeof(over->wsa_over));
				PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->wsa_over);
				break;
			}
			case OP_SAVE_USERDATA: {
				DB_OVER_EX* over = new DB_OVER_EX;
				over->op_mode = ev.event_id;
				over->object_id = ev.obj_id;
				strcpy_s(over->name, ev.name);
				ZeroMemory(&over->wsa_over, sizeof(over->wsa_over));
				PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->wsa_over);
				break;
			}
			default:
				break;
			}
		}
	}
}

void add_timer(int obj_id, int event_type, system_clock::time_point t)
{
	timer_l.lock();
	timer_queue.emplace(obj_id, event_type, t);
	timer_l.unlock();
}

void random_move_npc(int id);

void time_worker()
{
	while (true) {
		while (true) {
			event_type ev;
			timer_l.lock();
			if (false == timer_queue.empty()) {
				ev = timer_queue.top();
				if (ev.wakeup_time > system_clock::now()) {
					timer_l.unlock();
					break;
				}
				timer_queue.pop();
				timer_l.unlock();
			/*if (true == timer_queue.try_pop(ev)) {
				if (ev.wakeup_time > system_clock::now()) {
					timer_queue.emplace(ev);
					break;
				}*/
				switch (ev.event_id) {
				case OP_RANDOM_MOVE: {
					/*NODE* over = freelist.get_node();
					over->over.op_mode = OP_RANDOM_MOVE;
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->over.wsa_over);*/
					T_OVER_EX* over = new T_OVER_EX;
					over->op_mode = OP_RANDOM_MOVE;
					ZeroMemory(&over->wsa_over, sizeof(over->wsa_over));
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->wsa_over);
					break;
				}
				case OP_ATTACK_DELAY: {
					/*NODE* over = freelist.get_node();
					over->over.op_mode = OP_ATTACK_DELAY;
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->over.wsa_over);*/
					T_OVER_EX* over = new T_OVER_EX;
					over->op_mode = OP_ATTACK_DELAY;
					ZeroMemory(&over->wsa_over, sizeof(over->wsa_over));
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->wsa_over);
					break;
				}
				case OP_AUTO_RECOVERY: {
					/*NODE* over = freelist.get_node();
					over->over.op_mode = OP_AUTO_RECOVERY;
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->over.wsa_over);*/
					T_OVER_EX* over = new T_OVER_EX;
					over->op_mode = OP_AUTO_RECOVERY;
					ZeroMemory(&over->wsa_over, sizeof(over->wsa_over));
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->wsa_over);
					break;
				}
				case OP_RESURRECTION: {
					/*NODE* over = freelist.get_node();
					over->over.op_mode = OP_RESURRECTION;
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->over.wsa_over);*/
					T_OVER_EX* over = new T_OVER_EX;
					over->op_mode = OP_RESURRECTION;
					ZeroMemory(&over->wsa_over, sizeof(over->wsa_over));
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->wsa_over);
					break;
				}
				case OP_NPC_ACT: {
					T_OVER_EX* over = new T_OVER_EX;
					over->op_mode = OP_NPC_ACT;
					ZeroMemory(&over->wsa_over, sizeof(over->wsa_over));
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->wsa_over);
					break;
				}
				}
			}
			else {
				timer_l.unlock();
				break;
			}
		}
		this_thread::sleep_for(10ms);
	}
}

void wake_up_npc(int id)
{
	if (false == g_npcs[id - MAX_USER].is_active) {
		if (true == CAS(&g_npcs[id - MAX_USER].is_active, false, true))
			add_timer(id, OP_NPC_ACT, system_clock::now() + 1s);
	}
}

void error_display(const char* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	std::cout << msg;
	std::wcout << L"에러 " << lpMsgBuf << std::endl;
	LocalFree(lpMsgBuf);
}

bool is_npc(int p1)
{
	return p1 >= MAX_USER;
}

bool is_cl_near(int p1, int p2)
{
	int dist = (int)pow(g_clients[p1].x - g_clients[p2].x, 2) + (int)pow(g_clients[p1].y - g_clients[p2].y, 2);
	return dist <= VIEW_LIMIT * VIEW_LIMIT;
}

bool is_npc_near(int cl, int npc)
{
	int dist = (int)pow(g_clients[cl].x - g_npcs[npc - MAX_USER].x, 2) + (int)pow(g_clients[cl].y - g_npcs[npc - MAX_USER].y, 2);
	return dist <= VIEW_LIMIT * VIEW_LIMIT;
}

void is_near_sector(int id, unordered_set<pair<int, int>, pair_hash>* sector)
{
	short x, y;
	if (false == is_npc(id)) {
		x = g_clients[id].x;
		y = g_clients[id].y;
	}
	else {
		x = g_npcs[id - MAX_USER].x;
		y = g_npcs[id - MAX_USER].y;
	}

	int up = (y - VIEW_LIMIT) / SECTOR_HEIGHT;
	int vcenter = y / SECTOR_HEIGHT;
	int down = (y + VIEW_LIMIT) / SECTOR_HEIGHT;
	int left = (x - VIEW_LIMIT) / SECTOR_WIDTH;
	int hcenter = x / SECTOR_WIDTH;
	int right = (x + VIEW_LIMIT) / SECTOR_WIDTH;

	sector->emplace(up, left);
	sector->emplace(up, hcenter);
	sector->emplace(up, right);
	sector->emplace(vcenter, left);
	sector->emplace(vcenter, hcenter);
	sector->emplace(vcenter, right);
	sector->emplace(down, left);
	sector->emplace(down, hcenter);
	sector->emplace(down, right);
}

void send_packet(int id, void* p)
{
	unsigned char* packet = reinterpret_cast<unsigned char*>(p);
	NODE* send_over = freelist.get_node();
	send_over->over.op_mode = OP_MODE_SEND;
	memcpy(send_over->over.iocp_buf, packet, packet[0]);
	send_over->over.wsa_buf.len = packet[0];
	g_clients[id].c_lock.lock_shared();
	if (true == g_clients[id].is_active) {
		g_clients[id].c_lock.unlock_shared();
		WSASend(g_clients[id].m_sock, &send_over->over.wsa_buf, 1,
			NULL, 0, &send_over->over.wsa_over, NULL);
	}
	else g_clients[id].c_lock.unlock_shared();
}

void send_chat_packet(int to_client, int id, char* mess)
{
	sc_packet_chat p;
	size_t size = strlen(mess);
	p.id = id;
	p.size = sizeof(p) - 100 + size;
	p.type = SC_PACKET_CHAT;
	strncpy_s(p.message, mess, size);
	send_packet(to_client, &p);
}

void send_login_ok(int id)
{
	sc_packet_login_ok p;
	g_clients[id].c_lock.lock_shared();
	p.exp = g_clients[id].exp;
	p.hp = g_clients[id].hp;
	p.level = g_clients[id].level;
	g_clients[id].c_lock.unlock_shared();
	p.id = id;
	p.size = sizeof(p);
	p.type = SC_PACKET_LOGIN_OK;
	p.x = g_clients[id].x;
	p.y = g_clients[id].y;
	send_packet(id, &p);
}

void send_move_packet(int to_client, int id)
{
	sc_packet_move p;
	p.id = id;
	p.size = sizeof(p);
	p.type = SC_PACKET_MOVE;
	if (false == is_npc(id)) {
		p.x = g_clients[id].x;
		p.y = g_clients[id].y;
		p.move_time = g_clients[id].move_time;
	}
	else {
		p.x = g_npcs[id - MAX_USER].x;
		p.y = g_npcs[id - MAX_USER].y;
	}

	send_packet(to_client, &p);
}

void send_enter_packet(int to_client, int new_id)
{
	sc_packet_enter p;
	p.id = new_id;
	p.size = sizeof(p);
	p.type = SC_PACKET_ENTER;
	if (false == is_npc(new_id)) {
		p.x = g_clients[new_id].x;
		p.y = g_clients[new_id].y;
		g_clients[new_id].c_lock.lock_shared();
		strcpy_s(p.name, g_clients[new_id].name);
		g_clients[new_id].c_lock.unlock_shared();
		p.o_type = 0;
	}
	else {
		p.x = g_npcs[new_id - MAX_USER].x;
		p.y = g_npcs[new_id - MAX_USER].y;
		g_npcs[new_id - MAX_USER].c_lock.lock_shared();
		strcpy_s(p.name, g_npcs[new_id - MAX_USER].name);
		g_npcs[new_id - MAX_USER].c_lock.unlock_shared();
		p.o_type = g_npcs[new_id - MAX_USER].type;
	}

	send_packet(to_client, &p);
}

void send_leave_packet(int to_client, int id)
{
	sc_packet_leave p;
	p.id = id;
	p.size = sizeof(p);
	p.type = SC_PACKET_LEAVE;

	send_packet(to_client, &p);
}

void send_login_fail(int id)
{
	sc_packet_login_fail p;
	p.id = id;
	strcpy_s(p.message, "Already Connect");
	p.size = sizeof(p) - 100 + strlen(p.message);
	p.type = SC_PACKET_LOGIN_FAIL;

	send_packet(id, &p);
}

void send_stat_change_packet(int id)
{
	sc_packet_stat_change p;
	g_clients[id].c_lock.lock_shared();
	p.exp = g_clients[id].exp;
	p.hp = g_clients[id].hp;
	p.level = g_clients[id].level;
	g_clients[id].c_lock.unlock_shared();
	p.id = id;
	p.size = sizeof(p);
	p.type = SC_PACKET_STAT_CHANGE;

	send_packet(id, &p);
}

void send_monster_stat_change_packet_to_attacker(int id, int npc)
{
	sc_packet_stat_change p;
	g_npcs[npc - MAX_USER].c_lock.lock_shared();
	p.exp = g_clients[id].damage;	// 유저의 데미지
	p.hp = g_npcs[npc - MAX_USER].hp;
	p.level = g_npcs[npc - MAX_USER].level;
	g_npcs[npc - MAX_USER].c_lock.unlock_shared();
	p.id = npc;
	p.size = sizeof(p);
	p.type = SC_PACKET_STAT_CHANGE;

	send_packet(id, &p);
}

void process_move(int id, char dir)
{
	short old_y, new_y;
	old_y = new_y = g_clients[id].y;
	short old_x, new_x;
	old_x = new_x = g_clients[id].x;
	switch (dir) {
	case MV_UP: if (new_y > 0) new_y--; break;
	case MV_DOWN: if (new_y < (WORLD_HEIGHT - 1)) new_y++; break;
	case MV_LEFT: if (new_x > 0) new_x--; break;
	case MV_RIGHT: if (new_x < (WORLD_WIDTH - 1)) new_x++; break;
	default: cout << "Unknown Direction in CS_MOVE packet.\n";
		while (true);
	}
	g_clients[id].vl.lock_shared();
	unordered_set<int> old_viewlist = g_clients[id].view_list;
	g_clients[id].vl.unlock_shared();

	if ((old_y / SECTOR_HEIGHT == new_y / SECTOR_HEIGHT) && (old_x / SECTOR_WIDTH == new_x / SECTOR_WIDTH));
	else {
		int oy = old_y / SECTOR_HEIGHT;
		int ox = old_x / SECTOR_WIDTH;
		int ny = new_y / SECTOR_HEIGHT;
		int nx = new_x / SECTOR_WIDTH;
		sectors[oy][ox].s_lock.lock();
		sectors[oy][ox].sector.erase(id);
		sectors[oy][ox].s_lock.unlock();
		sectors[ny][nx].s_lock.lock();
		sectors[ny][nx].sector.emplace(id);
		sectors[ny][nx].s_lock.unlock();
	}

	g_clients[id].x = new_x;
	g_clients[id].y = new_y;

	send_move_packet(id, id);

	unordered_set<int> new_viewlist;
	unordered_set<pair<int, int>, pair_hash> s_id;
	is_near_sector(id, &s_id);
	for (const auto& pair : s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const auto& ob : sector) {
				if (id == ob) continue;
				if (false == is_npc(ob)) {
					if (false == g_clients[ob].is_active) continue;
					if (true == is_cl_near(id, ob)) new_viewlist.emplace(ob);
				}
				else {
					g_npcs[ob - MAX_USER].c_lock.lock_shared();
					if (false == g_npcs[ob - MAX_USER].live) {
						g_npcs[ob - MAX_USER].c_lock.unlock_shared();
						continue;
					}
					else g_npcs[ob - MAX_USER].c_lock.unlock_shared();
					if (true == is_npc_near(id, ob)) {
						new_viewlist.emplace(ob);
						wake_up_npc(ob);
					}
				}
			}
		}
	}

	for (const int& ob : new_viewlist) {
		if (0 == old_viewlist.count(ob)) {
			g_clients[id].vl.lock();
			g_clients[id].view_list.emplace(ob);
			g_clients[id].vl.unlock();
			send_enter_packet(id, ob);

			if (false == is_npc(ob)) {
				g_clients[ob].vl.lock();
				if (0 == g_clients[ob].view_list.count(id)) {
					g_clients[ob].view_list.emplace(id);
					g_clients[ob].vl.unlock();
					send_enter_packet(ob, id);
				}
				else {
					g_clients[ob].vl.unlock();
					send_move_packet(ob, id);
				}
			}
		}
		else {  // 이전에도 시야에 있었고, 이동후에도 시야에 있는 객체
			if (false == is_npc(ob)) {
				g_clients[ob].vl.lock();
				if (0 != g_clients[ob].view_list.count(id)) {
					g_clients[ob].vl.unlock();
					send_move_packet(ob, id);
				}
				else
				{
					g_clients[ob].view_list.emplace(id);
					g_clients[ob].vl.unlock();
					send_enter_packet(ob, id);
				}
			}
		}
	}

	for (const int& ob : old_viewlist) {
		if (0 == new_viewlist.count(ob)) {
			g_clients[id].vl.lock();
			g_clients[id].view_list.erase(ob);
			g_clients[id].vl.unlock();
			send_leave_packet(id, ob);
			if (false == is_npc(ob)) {
				g_clients[ob].vl.lock();
				if (0 != g_clients[ob].view_list.count(id)) {
					g_clients[ob].view_list.erase(id);
					g_clients[ob].vl.unlock();
					send_leave_packet(ob, id);
				}
				else g_clients[ob].vl.unlock();
			}
		}
	}
}

void process_chat(int id, char* message)
{
	short x = g_clients[id].x;
	short y = g_clients[id].y;

	unordered_set<pair<int, int>, pair_hash> s_id;
	is_near_sector(id, &s_id);

	for (const auto& pair : s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const auto& ob : sector) {
				if (false == is_npc(ob)) {
					if (false == g_clients[ob].is_active) continue;
					if (true == is_cl_near(id, ob)) send_chat_packet(ob, id, message);
				}
			}
		}
	}
}

void player_attack_npc(int p, int npc)
{
	short c_x = g_clients[p].x;
	short c_y = g_clients[p].y;

	short n_x = g_npcs[npc - MAX_USER].x;
	short n_y = g_npcs[npc - MAX_USER].y;

	int dist = (int)pow(c_x - n_x, 2) + (int)pow(c_y - n_y, 2);

	if (dist < 2) {
		g_npcs[npc - MAX_USER].c_lock.lock();
		g_npcs[npc - MAX_USER].hp -= g_clients[p].damage;
		if (g_npcs[npc - MAX_USER].hp <= 0) {
			g_npcs[npc - MAX_USER].live = false;
			int level = g_npcs[npc - MAX_USER].level;
			g_npcs[npc - MAX_USER].c_lock.unlock();
			g_clients[p].c_lock.lock();
			g_clients[p].exp += level * level * 2;
			if (g_clients[p].exp >= LEVEL_ONE_EXP * pow(REQ_EXP_MUL, g_clients[p].level)) {
				g_clients[p].exp -= LEVEL_ONE_EXP * pow(REQ_EXP_MUL, g_clients[p].level);
				g_clients[p].level += 1;
				g_clients[p].hp = g_clients[p].level * HEALTH_PER_LEVEL;
			}
			g_clients[p].c_lock.unlock();
			send_stat_change_packet(p);
			add_timer(npc, OP_RESURRECTION, system_clock::now() + 30s);
			g_npcs[npc - MAX_USER].lua_l.lock();
			lua_getglobal(g_npcs[npc - MAX_USER].L, "set_state");
			lua_pushnumber(g_npcs[npc - MAX_USER].L, 0);
			if (0 != lua_pcall(g_npcs[npc - MAX_USER].L, 1, 0, 0))
				error(g_npcs[npc - MAX_USER].L, "error running function 'XXX': %s\n", lua_tostring(g_npcs[npc - MAX_USER].L, -1));
			g_npcs[npc - MAX_USER].lua_l.unlock();
			send_monster_stat_change_packet_to_attacker(p, npc);
			return;
		}
		else g_npcs[npc - MAX_USER].c_lock.unlock();
		send_monster_stat_change_packet_to_attacker(p, npc);

		g_npcs[npc - MAX_USER].lua_l.lock();
		lua_getglobal(g_npcs[npc - MAX_USER].L, "damaged");
		lua_pushnumber(g_npcs[npc - MAX_USER].L, p);
		if (0 != lua_pcall(g_npcs[npc - MAX_USER].L, 1, 0, 0))
			error(g_npcs[npc - MAX_USER].L, "error running function 'XXX': %s\n", lua_tostring(g_npcs[npc - MAX_USER].L, -1));
		g_npcs[npc - MAX_USER].lua_l.unlock();
	}
}

void process_attack(int id)
{
	short x = g_clients[id].x;
	short y = g_clients[id].y;

	unordered_set<pair<int, int>, pair_hash> s_id;
	is_near_sector(id, &s_id);

	for (const auto& pair : s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const auto& ob : sector) {
				if (true == is_npc(ob)) {
					g_npcs[ob - MAX_USER].c_lock.lock_shared();
					if (false == g_npcs[ob - MAX_USER].live) {
						g_npcs[ob - MAX_USER].c_lock.unlock_shared();
						continue;
					}
					else g_npcs[ob - MAX_USER].c_lock.unlock_shared();
					player_attack_npc(id, ob);
				}
			}
		}
	}
}

void process_login_fail(int id)
{
	send_login_fail(id);

	g_clients[id].c_lock.lock();
	g_clients[id].is_active = false;
	closesocket(g_clients[id].m_sock);
	g_clients[id].m_sock = 0;
	g_clients[id].c_lock.unlock();
}

void process_packet(int id)
{
	char p_type = g_clients[id].m_packet_start[1];
	switch (p_type) {
	case CS_LOGIN: {
		cs_packet_login* p = reinterpret_cast<cs_packet_login*>(g_clients[id].m_packet_start);
		for (int i = 0; i < MAX_USER; ++i) {
			g_clients[i].c_lock.lock_shared();
			if (true == g_clients[i].is_active) {
				if (0 == strcmp(g_clients[i].name, p->name)) {
					g_clients[i].c_lock.unlock_shared();
					process_login_fail(id);
					goto skip;
				}
				else g_clients[i].c_lock.unlock_shared();
			}
			else g_clients[i].c_lock.unlock_shared();
		}
	/*	bool find = false;
		parallel_for(0, MAX_USER, [&id, &p, &find](int i) {
			g_clients[i].c_lock.lock_shared();
			if (true == g_clients[i].is_active) {
				if (0 == strcmp(g_clients[i].name, p->name)) {
					g_clients[i].c_lock.unlock_shared();
					process_login_fail(id);
					find = true;
				}
				else g_clients[i].c_lock.unlock_shared();
			}
			else g_clients[i].c_lock.unlock_shared();
			});*/
		//db_l.lock();
		db_queue.emplace(id, OP_GET_USERDATA, p->name);
		//db_l.unlock();
		skip:
		break;
	}
	case CS_MOVE: {
		cs_packet_move* p = reinterpret_cast<cs_packet_move*>(g_clients[id].m_packet_start);
		g_clients[id].move_time = p->move_time;
		process_move(id, p->direction);
		break;
	}
	case CS_CHAT: {
		cs_packet_chat* p = reinterpret_cast<cs_packet_chat*>(g_clients[id].m_packet_start);
		process_chat(id, p->message);
		break;
	}
	case CS_ATTACK: {
		g_clients[id].c_lock.lock_shared();
		if (true == g_clients[id].att_delay) {
			g_clients[id].c_lock.unlock_shared();
			break;
		}
		else g_clients[id].c_lock.unlock_shared();
		g_clients[id].c_lock.lock();
		g_clients[id].att_delay = true;
		g_clients[id].c_lock.unlock();
		add_timer(id, OP_ATTACK_DELAY, system_clock::now() + 1s);
		process_attack(id);
		break;
	}
	case CS_LOGOUT: {
		//db_l.lock();
		db_queue.emplace(id, OP_SAVE_USERDATA, g_clients[id].name);
		//db_l.unlock();
		break;
	}
	case CS_TELEORT: {
		unordered_set<pair<int, int>, pair_hash> s_id;
		is_near_sector(id, &s_id);
		for (auto pair : s_id) {
			if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
				sectors[pair.first][pair.second].s_lock.lock_shared();
				unordered_set<int> sector = sectors[pair.first][pair.second].sector;
				sectors[pair.first][pair.second].s_lock.unlock_shared();
				for (const int& ob : sector) {
					if (id == ob) continue;
					if (false == is_npc(ob)) {
						g_clients[ob].vl.lock();
						if (0 != g_clients[ob].view_list.count(id)) {
							g_clients[ob].view_list.erase(id);
							g_clients[ob].vl.unlock();
							send_leave_packet(ob, id);
						}
						else g_clients[ob].vl.unlock();
					}
				}
			}
		}
		sectors[g_clients[id].y / SECTOR_HEIGHT][g_clients[id].x / SECTOR_WIDTH].s_lock.lock();
		sectors[g_clients[id].y / SECTOR_HEIGHT][g_clients[id].x / SECTOR_WIDTH].sector.erase(id);
		sectors[g_clients[id].y / SECTOR_HEIGHT][g_clients[id].x / SECTOR_WIDTH].s_lock.unlock();
		g_clients[id].x = rand() % WORLD_WIDTH;
		g_clients[id].y = rand() % WORLD_HEIGHT;
		send_move_packet(id, id);
		sectors[g_clients[id].y / SECTOR_HEIGHT][g_clients[id].x / SECTOR_WIDTH].s_lock.lock();
		sectors[g_clients[id].y / SECTOR_HEIGHT][g_clients[id].x / SECTOR_WIDTH].sector.emplace(id);
		sectors[g_clients[id].y / SECTOR_HEIGHT][g_clients[id].x / SECTOR_WIDTH].s_lock.unlock();
		unordered_set<pair<int, int>, pair_hash> n_id;
		is_near_sector(id, &n_id);
		for (const auto& pair : n_id) {
			if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
				sectors[pair.first][pair.second].s_lock.lock_shared();
				unordered_set<int> sector = sectors[pair.first][pair.second].sector;
				sectors[pair.first][pair.second].s_lock.unlock_shared();
				for (const int& ob : sector) {
					if (id == ob) continue;
					if (false == is_npc(ob)) {
						if (false == g_clients[ob].is_active) continue;
						if (true == is_cl_near(id, ob)) {
							g_clients[ob].vl.lock();
							if (0 == g_clients[ob].view_list.count(id)) {
								g_clients[ob].view_list.emplace(id);
								g_clients[ob].vl.unlock();
								send_enter_packet(ob, id);
							}
							else g_clients[ob].vl.unlock();
							g_clients[id].vl.lock();
							if (0 == g_clients[id].view_list.count(ob)) {
								g_clients[id].view_list.emplace(ob);
								g_clients[id].vl.unlock();
								send_enter_packet(id, ob);
							}
							else g_clients[id].vl.unlock();
						}
					}
					else {
						g_npcs[ob - MAX_USER].c_lock.lock_shared();
						if (false == g_npcs[ob - MAX_USER].live) {
							g_npcs[ob - MAX_USER].c_lock.unlock_shared();
							continue;
						}
						else g_npcs[ob - MAX_USER].c_lock.unlock_shared();
						if (false == is_npc_near(id, ob)) continue;
						g_clients[id].vl.lock();
						g_clients[id].view_list.emplace(ob);
						g_clients[id].vl.unlock();
						send_enter_packet(id, ob);
						wake_up_npc(ob);
					}
				}
			}
		}
		break;
	}
	default:
		cout << "Unknown Packet type [" << p_type << "] from Client [" << id << "]\n";
		//while (true);
	}
}

constexpr int MIN_BUFF_SIZE = 1024;

void process_recv(int id, DWORD io_size)
{
	unsigned char p_size = g_clients[id].m_packet_start[0];
	unsigned char* next_recv_ptr = g_clients[id].m_recv_start + io_size;
	while (p_size <= next_recv_ptr - g_clients[id].m_packet_start) {
		process_packet(id);
		g_clients[id].m_packet_start += p_size;
		if (g_clients[id].m_packet_start < next_recv_ptr)
			p_size = g_clients[id].m_packet_start[0];
		else break;
	}

	long long left_data = next_recv_ptr - g_clients[id].m_packet_start;

	if (MAX_BUFFER - (next_recv_ptr - g_clients[id].m_recv_over.iocp_buf) < MIN_BUFF_SIZE) {
		memcpy(g_clients[id].m_recv_over.iocp_buf, g_clients[id].m_packet_start, left_data);
		g_clients[id].m_packet_start = g_clients[id].m_recv_over.iocp_buf;
		next_recv_ptr = g_clients[id].m_packet_start + left_data;
	}
	DWORD flags = 0;
	g_clients[id].m_recv_start = next_recv_ptr;
	ZeroMemory(&g_clients[id].m_recv_over.wsa_over, sizeof(WSAOVERLAPPED));
	g_clients[id].m_recv_over.wsa_buf.buf = reinterpret_cast<CHAR*>(next_recv_ptr);
	g_clients[id].m_recv_over.wsa_buf.len = static_cast<ULONG>(MAX_BUFFER - (next_recv_ptr - g_clients[id].m_recv_over.iocp_buf));

	g_clients[id].c_lock.lock_shared();
	if (true == g_clients[id].is_active) {
		g_clients[id].c_lock.unlock_shared();
		WSARecv(g_clients[id].m_sock, &g_clients[id].m_recv_over.wsa_buf, 1, NULL, &flags, &g_clients[id].m_recv_over.wsa_over, NULL);
	}
	else g_clients[id].c_lock.unlock_shared();
}

void add_new_client(SOCKET ns)
{
	int i;
	id_lock.lock();
	for (i = 0; i < MAX_USER; ++i)
		if (g_clients[i].is_active == false) break;
	id_lock.unlock();
	if (MAX_USER == i) {
		cout << "Max user limit exceeded.\n";
		closesocket(ns);
	}
	else {
		//cout << "New Client [" << i << "] Accepted" << endl;
		g_clients[i].c_lock.lock();
		g_clients[i].m_sock = ns;
		g_clients[i].is_active = true;
		g_clients[i].c_lock.unlock();

		CreateIoCompletionPort((HANDLE)g_clients[i].m_sock, h_iocp, i, 0);
		DWORD flags = 0;
		int ret = 0;
		g_clients[i].c_lock.lock_shared();
		if (g_clients[i].is_active == true) {
			g_clients[i].c_lock.unlock_shared();
			ret = WSARecv(g_clients[i].m_sock, &g_clients[i].m_recv_over.wsa_buf, 1, NULL,
				&flags, &g_clients[i].m_recv_over.wsa_over, NULL);
		}
		else g_clients[i].c_lock.unlock_shared();
		if (SOCKET_ERROR == ret)
			if (WSAGetLastError() != ERROR_IO_PENDING);
				//error_display("WSARecv : ", WSAGetLastError());
	}
	SOCKET cSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	g_accept_over.op_mode = OP_MODE_ACCEPT;
	g_accept_over.wsa_buf.len = static_cast<ULONG>(cSocket);
	ZeroMemory(&g_accept_over.wsa_over, sizeof(g_accept_over.wsa_over));
	AcceptEx(g_lSocket, cSocket, g_accept_over.iocp_buf, 0, 32, 32, NULL, &g_accept_over.wsa_over);
}

void disconnect_client(int id)
{
	unordered_set<pair<int, int>, pair_hash> s_id;
	is_near_sector(id, &s_id);
	for (auto pair : s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const int& ob : sector) {
				if (id == ob) continue;
				if (false == is_npc(ob)) {
					g_clients[ob].vl.lock();
					if (0 != g_clients[ob].view_list.count(id)) {
						g_clients[ob].view_list.erase(id);
						g_clients[ob].vl.unlock();
						send_leave_packet(ob, id);
					}
					else g_clients[ob].vl.unlock();
				}
			}
		}
	}
	sectors[g_clients[id].y / SECTOR_HEIGHT][g_clients[id].x / SECTOR_WIDTH].s_lock.lock();
	sectors[g_clients[id].y / SECTOR_HEIGHT][g_clients[id].x / SECTOR_WIDTH].sector.erase(id);
	sectors[g_clients[id].y / SECTOR_HEIGHT][g_clients[id].x / SECTOR_WIDTH].s_lock.unlock();
	g_clients[id].c_lock.lock();
	g_clients[id].is_active = false;
	closesocket(g_clients[id].m_sock);
	g_clients[id].m_sock = 0;
	g_clients[id].name[0] = '\0';
	g_clients[id].c_lock.unlock();
	g_clients[id].vl.lock();
	g_clients[id].view_list.clear();
	g_clients[id].vl.unlock();
}

void login_ok(int id, USER_DATA* u_data)
{
	g_clients[id].c_lock.lock();
	strcpy_s(g_clients[id].name, u_data->name);
	g_clients[id].level = u_data->level;
	g_clients[id].hp = u_data->hp;
	g_clients[id].exp = u_data->exp;
	g_clients[id].c_lock.unlock();

	if (false == g_clients[id].recovering) {
		if (true == CAS(&g_clients[id].recovering, false, true))
			add_timer(id, OP_AUTO_RECOVERY, system_clock::now() + 5s);
	}

	int x = g_clients[id].x = u_data->x;
	int y = g_clients[id].y = u_data->y;
	// 섹터에 추가
	sectors[y / SECTOR_HEIGHT][x / SECTOR_WIDTH].s_lock.lock();
	sectors[y / SECTOR_HEIGHT][x / SECTOR_WIDTH].sector.emplace(id);
	sectors[y / SECTOR_HEIGHT][x / SECTOR_WIDTH].s_lock.unlock();
	send_login_ok(id);
	unordered_set<pair<int, int>, pair_hash> s_id;
	is_near_sector(id, &s_id);
	for (const auto& pair : s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const int& ob : sector) {
				if (id == ob) continue;
				if (false == is_npc(ob)) {
					if (false == g_clients[ob].is_active) continue;
					if (true == is_cl_near(id, ob)) {
						g_clients[ob].vl.lock();
						if (0 == g_clients[ob].view_list.count(id)) {
							g_clients[ob].view_list.emplace(id);
							g_clients[ob].vl.unlock();
							send_enter_packet(ob, id);
						}
						else g_clients[ob].vl.unlock();
						g_clients[id].vl.lock();
						if (0 == g_clients[id].view_list.count(ob)) {
							g_clients[id].view_list.emplace(ob);
							g_clients[id].vl.unlock();
							send_enter_packet(id, ob);
						}
						else g_clients[id].vl.unlock();
					}
				}
				else {
					g_npcs[ob - MAX_USER].c_lock.lock_shared();
					if (false == g_npcs[ob - MAX_USER].live) {
						g_npcs[ob - MAX_USER].c_lock.unlock_shared();
						continue;
					}
					else g_npcs[ob - MAX_USER].c_lock.unlock_shared();
					if (false == is_npc_near(id, ob)) continue;
					g_clients[id].vl.lock();
					g_clients[id].view_list.emplace(ob);
					g_clients[id].vl.unlock();
					send_enter_packet(id, ob);
					wake_up_npc(ob);
				}
			}
		}
	}
}

void random_move_npc(int id)
{
	unordered_set<int> old_viewlist;
	unordered_set<pair<int, int>, pair_hash> old_s_id;
	is_near_sector(id, &old_s_id);
	for (const auto& pair : old_s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const int& ob : sector) {
				if (true == is_npc(ob)) continue;
				if (false == g_clients[ob].is_active) continue;
				if (true == is_npc_near(ob, id)) old_viewlist.emplace(ob);
			}
		}
	}
	short old_x, new_x;
	old_x = new_x = g_npcs[id - MAX_USER].x;
	short old_y, new_y;
	old_y = new_y = g_npcs[id - MAX_USER].y;

	switch (rand() % 4) {
	case 0: if (new_x > 0) --new_x; break;
	case 1: if (new_x < (WORLD_WIDTH - 1)) ++new_x; break;
	case 2: if (new_y > 0) --new_y; break;
	case 3: if (new_y < (WORLD_HEIGHT - 1)) ++new_y; break;
	}
	g_npcs[id - MAX_USER].x = new_x;
	g_npcs[id - MAX_USER].y = new_y;

	if ((old_y / SECTOR_HEIGHT == new_y / SECTOR_HEIGHT) && (old_x / SECTOR_WIDTH == new_x / SECTOR_WIDTH));
	else {
		int oy = old_y / SECTOR_HEIGHT;
		int ox = old_x / SECTOR_WIDTH;
		int ny = new_y / SECTOR_HEIGHT;
		int nx = new_x / SECTOR_WIDTH;
		sectors[oy][ox].s_lock.lock();
		sectors[oy][ox].sector.erase(id);
		sectors[oy][ox].s_lock.unlock();
		sectors[ny][nx].s_lock.lock();
		sectors[ny][nx].sector.emplace(id);
		sectors[ny][nx].s_lock.unlock();
	}

	unordered_set<int> new_viewlist;
	unordered_set<pair<int, int>, pair_hash> new_s_id;
	is_near_sector(id, &new_s_id);
	for (const auto& pair : new_s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const int& ob : sector) {
				if (true == is_npc(ob)) continue;
				if (false == g_clients[ob].is_active) continue;
				if (true == is_npc_near(ob, id)) new_viewlist.emplace(ob);
			}
		}
	}

	for (const auto& pl : old_viewlist) {
		if (0 != new_viewlist.count(pl)) {
			g_clients[pl].vl.lock();
			if (0 != g_clients[pl].view_list.count(id)) {
				g_clients[pl].vl.unlock();
				send_move_packet(pl, id);
			}
			else {
				g_clients[pl].view_list.emplace(id);
				g_clients[pl].vl.unlock();
				send_enter_packet(pl, id);
			}
		}
		else {
			g_clients[pl].vl.lock();
			if (0 != g_clients[pl].view_list.count(id)) {
				g_clients[pl].view_list.erase(id);
				g_clients[pl].vl.unlock();
				send_leave_packet(pl, id);
			}
			else g_clients[pl].vl.unlock();
		}
	}

	for (const auto& pl : new_viewlist) {
		if (0 != old_viewlist.count(pl)) {
			g_clients[pl].vl.lock();
			if (0 == g_clients[pl].view_list.count(id)) {
				g_clients[pl].view_list.emplace(id);
				g_clients[pl].vl.unlock();
				send_enter_packet(pl, id);
			}
			else {
				g_clients[pl].vl.unlock();
				send_move_packet(pl, id);
			}
		}
	}

	if (true == new_viewlist.empty()) {
		g_npcs[id - MAX_USER].c_lock.lock();
		g_npcs[id - MAX_USER].is_active = false;
		g_npcs[id - MAX_USER].c_lock.unlock();
	}
	else {
		add_timer(id, OP_NPC_ACT, system_clock::now() + 1s);
	}
}

void npc_attack_player(int p, int npc)
{
	g_clients[p].c_lock.lock();
	g_clients[p].hp -= g_npcs[npc - MAX_USER].damage;
	if (g_clients[p].hp <= 0) {
		g_clients[p].hp = g_clients[p].level * HEALTH_PER_LEVEL;
		g_clients[p].exp /= 2;
		g_clients[p].c_lock.unlock();
		unordered_set<pair<int, int>, pair_hash> s_id;
		is_near_sector(p, &s_id);
		for (auto pair : s_id) {
			if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
				sectors[pair.first][pair.second].s_lock.lock_shared();
				unordered_set<int> sector = sectors[pair.first][pair.second].sector;
				sectors[pair.first][pair.second].s_lock.unlock_shared();
				for (const int& ob : sector) {
					if (p == ob) continue;
					if (false == is_npc(ob)) {
						g_clients[ob].vl.lock();
						if (0 != g_clients[ob].view_list.count(p)) {
							g_clients[ob].view_list.erase(p);
							g_clients[ob].vl.unlock();
							send_leave_packet(ob, p);
						}
						else g_clients[ob].vl.unlock();
					}
				}
			}
		}
		sectors[g_clients[p].y / SECTOR_HEIGHT][g_clients[p].x / SECTOR_WIDTH].s_lock.lock();
		sectors[g_clients[p].y / SECTOR_HEIGHT][g_clients[p].x / SECTOR_WIDTH].sector.erase(p);
		sectors[g_clients[p].y / SECTOR_HEIGHT][g_clients[p].x / SECTOR_WIDTH].s_lock.unlock();
		g_clients[p].x = 400;
		g_clients[p].y = 400;
		send_move_packet(p, p);
		send_stat_change_packet(p);
		sectors[g_clients[p].y / SECTOR_HEIGHT][g_clients[p].x / SECTOR_WIDTH].s_lock.lock();
		sectors[g_clients[p].y / SECTOR_HEIGHT][g_clients[p].x / SECTOR_WIDTH].sector.emplace(p);
		sectors[g_clients[p].y / SECTOR_HEIGHT][g_clients[p].x / SECTOR_WIDTH].s_lock.unlock();
		unordered_set<pair<int, int>, pair_hash> n_id;
		is_near_sector(p, &n_id);
		for (const auto& pair : n_id) {
			if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
				sectors[pair.first][pair.second].s_lock.lock_shared();
				unordered_set<int> sector = sectors[pair.first][pair.second].sector;
				sectors[pair.first][pair.second].s_lock.unlock_shared();
				for (const int& ob : sector) {
					if (p == ob) continue;
					if (false == is_npc(ob)) {
						if (false == g_clients[ob].is_active) continue;
						if (true == is_cl_near(p, ob)) {
							g_clients[ob].vl.lock();
							if (0 == g_clients[ob].view_list.count(p)) {
								g_clients[ob].view_list.emplace(p);
								g_clients[ob].vl.unlock();
								send_enter_packet(ob, p);
							}
							else g_clients[ob].vl.unlock();
							g_clients[p].vl.lock();
							if (0 == g_clients[p].view_list.count(ob)) {
								g_clients[p].view_list.emplace(ob);
								g_clients[p].vl.unlock();
								send_enter_packet(p, ob);
							}
							else g_clients[p].vl.unlock();
						}
					}
					else {
						g_npcs[ob - MAX_USER].c_lock.lock_shared();
						if (false == g_npcs[ob - MAX_USER].live) {
							g_npcs[ob - MAX_USER].c_lock.unlock_shared();
							continue;
						}
						else g_npcs[ob - MAX_USER].c_lock.unlock_shared();
						if (false == is_npc_near(p, ob)) continue;
						g_clients[p].vl.lock();
						g_clients[p].view_list.emplace(ob);
						g_clients[p].vl.unlock();
						send_enter_packet(p, ob);
						wake_up_npc(ob);
					}
				}
			}
		}
	}
	else g_clients[p].c_lock.unlock();
	if (false == g_clients[p].recovering) {
		if (true == CAS(&g_clients[p].recovering, false, true))
			add_timer(p, OP_AUTO_RECOVERY, system_clock::now() + 5s);
	}
	send_stat_change_packet(p);

	unordered_set<int> new_viewlist;
	unordered_set<pair<int, int>, pair_hash> new_s_id;
	is_near_sector(npc, &new_s_id);
	for (const auto& pair : new_s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const int& ob : sector) {
				if (true == is_npc(ob)) continue;
				if (false == g_clients[ob].is_active) continue;
				if (true == is_npc_near(ob, npc)) new_viewlist.emplace(ob);
			}
		}
	}
	if (true == new_viewlist.empty()) {
		g_npcs[npc - MAX_USER].c_lock.lock();
		g_npcs[npc - MAX_USER].is_active = false;
		g_npcs[npc - MAX_USER].c_lock.unlock();
	}
	else {
		add_timer(npc, OP_NPC_ACT, system_clock::now() + 1s);
	}
}

void random_move_npc_rect(int id, int fixed_x, int fixed_y,int range)
{
	unordered_set<int> old_viewlist;
	unordered_set<pair<int, int>, pair_hash> old_s_id;
	is_near_sector(id, &old_s_id);
	for (const auto& pair : old_s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const int& ob : sector) {
				if (true == is_npc(ob)) continue;
				if (false == g_clients[ob].is_active) continue;
				if (true == is_npc_near(ob, id)) old_viewlist.emplace(ob);
			}
		}
	}
	short old_x, new_x;
	old_x = new_x = g_npcs[id - MAX_USER].x;
	short old_y, new_y;
	old_y = new_y = g_npcs[id - MAX_USER].y;

	switch (rand() % 4) {
	case 0: if (new_x > 0 && new_x > (fixed_x - range)) --new_x; break;
	case 1: if (new_x < (WORLD_WIDTH - 1) && new_x < (fixed_x + range)) ++new_x; break;
	case 2: if (new_y > 0 && new_y > (fixed_y - range)) --new_y; break;
	case 3: if (new_y < (WORLD_HEIGHT - 1) && new_y < (fixed_y + range)) ++new_y; break;
	}
	g_npcs[id - MAX_USER].x = new_x;
	g_npcs[id - MAX_USER].y = new_y;

	if ((old_y / SECTOR_HEIGHT == new_y / SECTOR_HEIGHT) && (old_x / SECTOR_WIDTH == new_x / SECTOR_WIDTH));
	else {
		int oy = old_y / SECTOR_HEIGHT;
		int ox = old_x / SECTOR_WIDTH;
		int ny = new_y / SECTOR_HEIGHT;
		int nx = new_x / SECTOR_WIDTH;
		sectors[oy][ox].s_lock.lock();
		sectors[oy][ox].sector.erase(id);
		sectors[oy][ox].s_lock.unlock();
		sectors[ny][nx].s_lock.lock();
		sectors[ny][nx].sector.emplace(id);
		sectors[ny][nx].s_lock.unlock();
	}

	unordered_set<int> new_viewlist;
	unordered_set<pair<int, int>, pair_hash> new_s_id;
	is_near_sector(id, &new_s_id);
	for (const auto& pair : new_s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const int& ob : sector) {
				if (true == is_npc(ob)) continue;
				if (false == g_clients[ob].is_active) continue;
				if (true == is_npc_near(ob, id)) new_viewlist.emplace(ob);
			}
		}
	}

	for (const auto& pl : old_viewlist) {
		if (0 != new_viewlist.count(pl)) {
			g_clients[pl].vl.lock();
			if (0 != g_clients[pl].view_list.count(id)) {
				g_clients[pl].vl.unlock();
				send_move_packet(pl, id);
			}
			else {
				g_clients[pl].view_list.emplace(id);
				g_clients[pl].vl.unlock();
				send_enter_packet(pl, id);
			}
		}
		else {
			g_clients[pl].vl.lock();
			if (0 != g_clients[pl].view_list.count(id)) {
				g_clients[pl].view_list.erase(id);
				g_clients[pl].vl.unlock();
				send_leave_packet(pl, id);
			}
			else g_clients[pl].vl.unlock();
		}
	}

	for (const auto& pl : new_viewlist) {
		if (0 != old_viewlist.count(pl)) {
			g_clients[pl].vl.lock();
			if (0 == g_clients[pl].view_list.count(id)) {
				g_clients[pl].view_list.emplace(id);
				g_clients[pl].vl.unlock();
				send_enter_packet(pl, id);
			}
			else {
				g_clients[pl].vl.unlock();
				send_move_packet(pl, id);
			}
		}
	}

	if (true == new_viewlist.empty()) {
		g_npcs[id - MAX_USER].c_lock.lock();
		g_npcs[id - MAX_USER].is_active = false;
		g_npcs[id - MAX_USER].c_lock.unlock();
	}
	else {
		add_timer(id, OP_NPC_ACT, system_clock::now() + 1s);
	}
}

void npc_move_to_client(int id, int player)
{
	unordered_set<int> old_viewlist;
	unordered_set<pair<int, int>, pair_hash> old_s_id;
	is_near_sector(id, &old_s_id);
	for (const auto& pair : old_s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const int& ob : sector) {
				if (true == is_npc(ob)) continue;
				if (false == g_clients[ob].is_active) continue;
				if (true == is_npc_near(ob, id)) old_viewlist.emplace(ob);
			}
		}
	}

	short old_x, new_x;
	old_x = new_x = g_npcs[id - MAX_USER].x;
	short old_y, new_y;
	old_y = new_y = g_npcs[id - MAX_USER].y;
	short px = g_clients[player].x;
	short py = g_clients[player].y;

	if (new_x < px)
		++new_x;
	else if (px < new_x)
		--new_x;
	else if (new_y < py)
		++new_y;
	else if (py < new_y)
		--new_y;

	g_npcs[id - MAX_USER].x = new_x;
	g_npcs[id - MAX_USER].y = new_y;

	if ((old_y / SECTOR_HEIGHT == new_y / SECTOR_HEIGHT) && (old_x / SECTOR_WIDTH == new_x / SECTOR_WIDTH));
	else {
		int oy = old_y / SECTOR_HEIGHT;
		int ox = old_x / SECTOR_WIDTH;
		int ny = new_y / SECTOR_HEIGHT;
		int nx = new_x / SECTOR_WIDTH;
		sectors[oy][ox].s_lock.lock();
		sectors[oy][ox].sector.erase(id);
		sectors[oy][ox].s_lock.unlock();
		sectors[ny][nx].s_lock.lock();
		sectors[ny][nx].sector.emplace(id);
		sectors[ny][nx].s_lock.unlock();
	}

	unordered_set<int> new_viewlist;
	unordered_set<pair<int, int>, pair_hash> new_s_id;
	is_near_sector(id, &new_s_id);
	for (const auto& pair : new_s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const int& ob : sector) {
				if (true == is_npc(ob)) continue;
				if (false == g_clients[ob].is_active) continue;
				if (true == is_npc_near(ob, id)) new_viewlist.emplace(ob);
			}
		}
	}

	for (const auto& pl : old_viewlist) {
		if (0 != new_viewlist.count(pl)) {
			g_clients[pl].vl.lock();
			if (0 != g_clients[pl].view_list.count(id)) {
				g_clients[pl].vl.unlock();
				send_move_packet(pl, id);
			}
			else {
				g_clients[pl].view_list.emplace(id);
				g_clients[pl].vl.unlock();
				send_enter_packet(pl, id);
			}
		}
		else {
			g_clients[pl].vl.lock();
			if (0 != g_clients[pl].view_list.count(id)) {
				g_clients[pl].view_list.erase(id);
				g_clients[pl].vl.unlock();
				send_leave_packet(pl, id);
			}
			else g_clients[pl].vl.unlock();
		}
	}

	for (const auto& pl : new_viewlist) {
		if (0 != old_viewlist.count(pl)) {
			g_clients[pl].vl.lock();
			if (0 == g_clients[pl].view_list.count(id)) {
				g_clients[pl].view_list.emplace(id);
				g_clients[pl].vl.unlock();
				send_enter_packet(pl, id);
			}
			else {
				g_clients[pl].vl.unlock();
				send_move_packet(pl, id);
			}
		}
	}

	if (true == new_viewlist.empty()) {
		g_npcs[id - MAX_USER].c_lock.lock();
		g_npcs[id - MAX_USER].is_active = false;
		g_npcs[id - MAX_USER].c_lock.unlock();
	}
	else {
		add_timer(id, OP_NPC_ACT, system_clock::now() + 1s);
	}
}

void act_npc(int id)
{
	npc_info* npc = &g_npcs[id - MAX_USER];
	npc->lua_l.lock();
	lua_getglobal(npc->L, "act");
	if (0 != lua_pcall(npc->L, 0, 0, 0))
		error(npc->L, "error running function 'XXX': %s\n", lua_tostring(npc->L, -1));
	npc->lua_l.unlock();
}

int API_SendMessage(lua_State* L)
{
	int my_id = (int)lua_tointeger(L, -3);
	int user_id = (int)lua_tointeger(L, -2);
	char* mess = (char*)lua_tostring(L, -1);

	lua_pop(L, 4);

	send_chat_packet(user_id, my_id, mess);

	return 0;
}

int API_get_x(lua_State* L)
{
	int user_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int x;
	if (false == is_npc(user_id))
		x = g_clients[user_id].x;
	else x = g_npcs[user_id - MAX_USER].x;
	lua_pushnumber(L, x);

	return 1;
}

int API_get_y(lua_State* L)
{
	int user_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int y;
	if (false == is_npc(user_id))
		y = g_clients[user_id].y;
	else y = g_npcs[user_id - MAX_USER].y;
	lua_pushnumber(L, y);

	return 1;
}

int API_addTimer(lua_State* L)
{
	int my_id = (int)lua_tointeger(L, -3);
	int event_type = (int)lua_tointeger(L, -2);
	int wakeup_time = (int)lua_tointeger(L, -1);

	lua_pop(L, 4);

	add_timer(my_id, event_type, system_clock::now() + seconds{ wakeup_time });

	return 0;
}

int API_random_move(lua_State* L)
{
	int npc_id = (int)lua_tointeger(L, -4);
	int fixed_x = (int)lua_tointeger(L, -3);
	int fixed_y = (int)lua_tointeger(L, -2);
	int range = (int)lua_tointeger(L, -1);
	lua_pop(L, 5);

	random_move_npc_rect(npc_id, fixed_x, fixed_y, range);

	return 0;
}

int API_npc_move_to_player(lua_State* L)
{
	int npc_id = (int)lua_tointeger(L, -2);
	int player = (int)lua_tointeger(L, -1);
	lua_pop(L, 3);

	npc_move_to_client(npc_id, player);

	return 0;
}

int API_attack_player(lua_State* L)
{
	int npc_id = (int)lua_tointeger(L, -2);
	int player = (int)lua_tointeger(L, -1);
	lua_pop(L, 3);

	npc_attack_player(player, npc_id);

	return 0;
}

int API_look_around(lua_State* L)
{
	int id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);

	unordered_set<pair<int, int>, pair_hash> new_s_id;
	is_near_sector(id, &new_s_id);
	for (const auto& pair : new_s_id) {
		if ((0 <= pair.first && pair.first <= (WORLD_HEIGHT / SECTOR_HEIGHT) - 1) && (0 <= pair.second && pair.second <= (WORLD_WIDTH / SECTOR_WIDTH) - 1)) {
			sectors[pair.first][pair.second].s_lock.lock_shared();
			unordered_set<int> sector = sectors[pair.first][pair.second].sector;
			sectors[pair.first][pair.second].s_lock.unlock_shared();
			for (const int& ob : sector) {
				if (true == is_npc(ob)) continue;
				if (false == g_clients[ob].is_active) continue;
				short px = g_clients[ob].x;
				short py = g_clients[ob].y;
				short nx = g_npcs[id - MAX_USER].x;
				short ny = g_npcs[id - MAX_USER].y;
				int dist = (int)pow(px - nx, 2) + (int)pow(py - ny, 2);
				if (dist <= 25) {
					lua_pushnumber(L, ob);
					goto skip_look_around;
				}
			}
		}
	}
	lua_pushnumber(L, -1);
	skip_look_around:

	return 1;
}

void worker_thread()
{
	// 반복
	// 이 쓰레드를 IOCP thread pool에 등록 => GQCS
	// IOCP가 처리를 맞긴 I/O완료 데이터를 꺼내기
	// 꺼낸 I/O완료 데이터를 처리
	while (true) {
		DWORD io_size;
		int key;
		ULONG_PTR iocp_key;
		WSAOVERLAPPED* lpover;
		int ret = GetQueuedCompletionStatus(h_iocp, &io_size, &iocp_key, &lpover, INFINITE);
		key = static_cast<int>(iocp_key);
		//cout << "Completion detected" << endl;
		if (FALSE == ret) {
			//error_display("GQCS Error: ", WSAGetLastError());
		}

		OVER_EX* over_ex = reinterpret_cast<OVER_EX*>(lpover);
		switch (over_ex->op_mode) {
		case OP_MODE_ACCEPT: {
			add_new_client(static_cast<SOCKET>(over_ex->wsa_buf.len));
			break;
		}
		case OP_MODE_RECV: {
			if (io_size == 0) {
				g_clients[key].c_lock.lock_shared();
				if (false == g_clients[key].is_active) {
					g_clients[key].c_lock.unlock_shared();
					db_queue.emplace(key, OP_SAVE_USERDATA, g_clients[key].name);
				}
				else g_clients[key].c_lock.unlock_shared();
			}
			else {
				//cout << "Packet from Client [" << key << "]" << endl;
				process_recv(key, io_size);
			}
			break;
		}
		case OP_MODE_SEND: {
			freelist.free_node(reinterpret_cast<NODE*>(over_ex));
			if (io_size == 0) {
				g_clients[key].c_lock.lock_shared();
				if (false == g_clients[key].is_active) {
					g_clients[key].c_lock.unlock_shared();
					db_queue.emplace(key, OP_SAVE_USERDATA, g_clients[key].name);
				}
				else g_clients[key].c_lock.unlock_shared();
			}
			break;
		}
		case OP_RANDOM_MOVE: {
			T_OVER_EX* t_over_ex = reinterpret_cast<T_OVER_EX*>(lpover);
			random_move_npc(key);
			delete t_over_ex;
			//freelist.free_node(reinterpret_cast<NODE*>(over_ex));
			break;
		}
		case OP_GET_USERDATA: {
			DB_OVER_EX* db_over_ex = reinterpret_cast<DB_OVER_EX*>(lpover);
			SQLCHAR level;
			SQLINTEGER hp, exp;
			SQLSMALLINT x, y;
			SQLLEN cbLevel, cbHP, cbExp, cbX, cbY;

			SQLWCHAR uName[MAX_ID_LEN];
			//int n = MultiByteToWideChar(CP_ACP, 0, ev.name, -1, 0, 0);
			MultiByteToWideChar(CP_ACP, 0, db_over_ex->name, -1, uName, MAX_ID_LEN);
			uName[strlen(db_over_ex->name)] = '\0';
			SQLLEN cbValue = SQL_NTS;
			retcode = SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WCHAR, MAX_ID_LEN, 0, uName, strlen(db_over_ex->name), &cbValue);
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				retcode = SQLExecDirect(hstmt, (SQLWCHAR*)L"EXEC get_user_data ?", SQL_NTS);
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					//cout << "Exec OK!\n";
					SQLBindCol(hstmt, 1, SQL_C_UTINYINT, &level, 100, &cbLevel);
					SQLBindCol(hstmt, 2, SQL_C_LONG, &hp, 100, &cbHP);
					SQLBindCol(hstmt, 3, SQL_C_LONG, &exp, 100, &cbExp);
					SQLBindCol(hstmt, 4, SQL_C_SHORT, &x, 100, &cbX);
					SQLBindCol(hstmt, 5, SQL_C_SHORT, &y, 100, &cbY);

					// Fetch and print each row of data. On an error, display a message and exit.  
					//for (int i = 0; ; i++) {
					/*retcode = SQLFetch(hstmt);
					SQLCloseCursor(hstmt);*/
					while (SQL_NO_DATA != SQLFetch(hstmt)) {
						
					}
					SQLCloseCursor(hstmt);
					USER_DATA ud;
					ud.exp = exp;
					ud.hp = hp;
					ud.level = level;
					ud.x = x;
					ud.y = y;
					strcpy_s(ud.name, db_over_ex->name);
					login_ok(db_over_ex->object_id, &ud);
					//if (retcode == SQL_ERROR || retcode == SQL_SUCCESS_WITH_INFO)
					//	show_error(hstmt, SQL_HANDLE_STMT, retcode);
					//if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO)
					//{
					//	/*NODE* over = freelist.get_node();
					//	over->over.op_mode = ev.event_id;
					//	over->over.u_data.exp = exp;
					//	over->over.u_data.hp = hp;
					//	over->over.u_data.level = level;
					//	over->over.u_data.x = x;
					//	over->over.u_data.y = y;
					//	strcpy_s(over->over.u_data.name, ev.name);
					//	PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->over.wsa_over);*/
					//	/*OVER_EX* over = new OVER_EX;
					//	over->op_mode = ev.event_id;
					//	over->u_data.exp = exp;
					//	over->u_data.hp = hp;
					//	over->u_data.level = level;
					//	over->u_data.x = x;
					//	over->u_data.y = y;
					//	strcpy_s(over->u_data.name, ev.name);
					//	PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->wsa_over);*/
					//	USER_DATA ud;
					//	ud.exp = exp;
					//	ud.hp = hp;
					//	ud.level = level;
					//	ud.x = x;
					//	ud.y = y;
					//	strcpy_s(ud.name, db_over_ex->name);
					//	login_ok(db_over_ex->object_id, &ud);
					//	//int len = WideCharToMultiByte(CP_ACP, 0, szUser_name, -1, NULL, 0, NULL, NULL);
					//	//WideCharToMultiByte(CP_ACP, 0, szUser_name, -1, over->user_data->m_name, MAX_ID_LEN, NULL, NULL);
					//}
					////}
				}
				else show_error(hstmt, SQL_HANDLE_STMT, retcode);
			}
			else show_error(hstmt, SQL_HANDLE_STMT, retcode);
			delete db_over_ex;
			//freelist.free_node(reinterpret_cast<NODE*>(over_ex));
			break;
		}
		case OP_SAVE_USERDATA: {
			DB_OVER_EX* db_over_ex = reinterpret_cast<DB_OVER_EX*>(lpover);
			client_info* cl = &g_clients[key];
			cl->c_lock.lock_shared();
			SQLCHAR level = cl->level;
			SQLINTEGER hp = cl->hp;
			SQLINTEGER exp = cl->exp;
			cl->c_lock.unlock_shared();
			SQLSMALLINT x = cl->x;
			SQLSMALLINT y = cl->y;
			SQLWCHAR name[MAX_ID_LEN];
			MultiByteToWideChar(CP_ACP, 0, db_over_ex->name, -1, name, MAX_ID_LEN);

			SQLLEN cbValue = SQL_NTS;
			retcode = SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WCHAR, MAX_ID_LEN, 0, name, strlen(db_over_ex->name), &cbValue);
			retcode = SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT, 0, 0, &level, 0, 0);
			retcode = SQLBindParameter(hstmt, 3, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &hp, 0, 0);
			retcode = SQLBindParameter(hstmt, 4, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &exp, 0, 0);
			retcode = SQLBindParameter(hstmt, 5, SQL_PARAM_INPUT, SQL_C_SHORT, SQL_SMALLINT, 0, 0, &x, 0, 0);
			retcode = SQLBindParameter(hstmt, 6, SQL_PARAM_INPUT, SQL_C_SHORT, SQL_SMALLINT, 0, 0, &y, 0, 0);
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				retcode = SQLExecDirect(hstmt, (SQLWCHAR*)L"EXEC save_user_data ?, ?, ?, ?, ?, ?", SQL_NTS);
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					//cout << "Exec OK!\n";
					/*NODE* over = freelist.get_node();
					over->over.op_mode = ev.event_id;
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->over.wsa_over);*/
					/*OVER_EX* over = new OVER_EX;
					over->op_mode = ev.event_id;
					PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &over->wsa_over);*/
					disconnect_client(db_over_ex->object_id);
				}
				else show_error(hstmt, SQL_HANDLE_STMT, retcode);
			}
			else show_error(hstmt, SQL_HANDLE_STMT, retcode);
			delete db_over_ex;
			//freelist.free_node(reinterpret_cast<NODE*>(over_ex));
			break;
		}
		case OP_ATTACK_DELAY: {
			T_OVER_EX* t_over_ex = reinterpret_cast<T_OVER_EX*>(lpover);
			if (false == is_npc(key)) {
				g_clients[key].c_lock.lock();
				g_clients[key].att_delay = false;
				g_clients[key].c_lock.unlock();
			}
			else {
				g_npcs[key - MAX_USER].c_lock.lock();
				g_npcs[key - MAX_USER].att_delay = false;
				g_npcs[key - MAX_USER].c_lock.unlock();
			}
			delete t_over_ex;
			//freelist.free_node(reinterpret_cast<NODE*>(over_ex));
			break;
		}
		case OP_AUTO_RECOVERY: {
			T_OVER_EX* t_over_ex = reinterpret_cast<T_OVER_EX*>(lpover);
			g_clients[key].c_lock.lock();
			if (true == g_clients[key].is_active) {
				g_clients[key].hp += g_clients[key].level * HEALTH_PER_LEVEL / 10;
				if (g_clients[key].hp < g_clients[key].level * HEALTH_PER_LEVEL)
					add_timer(key, OP_AUTO_RECOVERY, system_clock::now() + 5s);
				else {
					g_clients[key].hp = g_clients[key].level * HEALTH_PER_LEVEL;
					g_clients[key].recovering = false;
				}
			}
			g_clients[key].c_lock.unlock();
			send_stat_change_packet(key);
			delete t_over_ex;
			//freelist.free_node(reinterpret_cast<NODE*>(over_ex));
			break;
		}
		case OP_RESURRECTION: {
			T_OVER_EX* t_over_ex = reinterpret_cast<T_OVER_EX*>(lpover);
			g_npcs[key - MAX_USER].c_lock.lock();
			g_npcs[key - MAX_USER].hp = g_npcs[key - MAX_USER].level * HEALTH_PER_LEVEL;
			g_npcs[key - MAX_USER].live = true;
			g_npcs[key - MAX_USER].c_lock.unlock();
			delete t_over_ex;
			//freelist.free_node(reinterpret_cast<NODE*>(over_ex));
			break;
		}
		case OP_NPC_ACT: {
			T_OVER_EX* t_over_ex = reinterpret_cast<T_OVER_EX*>(lpover);
			act_npc(key);
			delete t_over_ex;
			break;
		}
		}
	}

}

void initialize_NPC()
{
	cout << "Initailizing NPCs\n";
	for (int i = 0; i < NUM_NPC; ++i) {
		short x = rand() % WORLD_WIDTH;
		short y = rand() % WORLD_HEIGHT;
		g_npcs[i].x = x;
		g_npcs[i].y = y;
		sectors[g_npcs[i].y / SECTOR_HEIGHT][g_npcs[i].x / SECTOR_WIDTH].sector.emplace(i + MAX_USER);
		g_npcs[i].is_active = false;
		g_npcs[i].live = true;
		g_npcs[i].level = (i % 5) + 1;
		g_npcs[i].hp = HEALTH_PER_LEVEL * g_npcs[i].level;

		lua_State* L = g_npcs[i].L = luaL_newstate();
		luaL_openlibs(L);

		luaL_loadfile(L, "monster.lua");
		if (0 != lua_pcall(L, 0, 0, 0))
			error(L, "error running function 'XXX': %s\n", lua_tostring(L, -1));

		char npc_name[MAX_ID_LEN];
		lua_getglobal(L, "init");
		if (i < 33333) {
			sprintf_s(npc_name, "S%d", i);
			strcpy_s(g_npcs[i].name, npc_name);
			g_npcs[i].type = 0;
			lua_pushnumber(L, i + MAX_USER);
			lua_pushnumber(L, 0);
			lua_pushnumber(L, 0);
			lua_pushnumber(L, x);
			lua_pushnumber(L, y);
		}
		else if (i < 66666) {
			sprintf_s(npc_name, "D%d", i);
			strcpy_s(g_npcs[i].name, npc_name);
			g_npcs[i].type = 1;
			lua_pushnumber(L, i + MAX_USER);
			lua_pushnumber(L, 1);
			lua_pushnumber(L, 0);
			lua_pushnumber(L, x);
			lua_pushnumber(L, y);
		}
		else if (i < 100000) {
			sprintf_s(npc_name, "W%d", i);
			strcpy_s(g_npcs[i].name, npc_name);
			g_npcs[i].type = 2;
			lua_pushnumber(L, i + MAX_USER);
			lua_pushnumber(L, 2);
			lua_pushnumber(L, 0);
			lua_pushnumber(L, x);
			lua_pushnumber(L, y);
		}
		if (0 != lua_pcall(L, 5, 0, 0))
			error(L, "error running function 'XXX': %s\n", lua_tostring(L, -1));

		lua_register(L, "API_SendMessage", API_SendMessage);
		lua_register(L, "API_get_x", API_get_x);
		lua_register(L, "API_get_y", API_get_y);
		lua_register(L, "API_addTimer", API_addTimer);
		lua_register(L, "API_random_move", API_random_move);
		lua_register(L, "API_npc_move_to_player", API_npc_move_to_player);
		lua_register(L, "API_attack_player", API_attack_player);
		lua_register(L, "API_look_around", API_look_around);
	}
	cout << "NPC Initialize Finish\n";
}

void initialize_clients()
{
	cout << "Initailizing clients\n";
	for (int i = 0; i < MAX_USER; ++i) {
		g_clients[i].m_packet_start = g_clients[i].m_recv_over.iocp_buf;
		g_clients[i].m_recv_over.op_mode = OP_MODE_RECV;
		g_clients[i].m_recv_over.wsa_buf.buf
			= reinterpret_cast<CHAR*>(g_clients[i].m_recv_over.iocp_buf);
		g_clients[i].m_recv_over.wsa_buf.len = sizeof(g_clients[i].m_recv_over.iocp_buf);
		ZeroMemory(&g_clients[i].m_recv_over.wsa_over, sizeof(g_clients[i].m_recv_over.wsa_over));
		g_clients[i].m_recv_start = g_clients[i].m_recv_over.iocp_buf;
	}
	cout << "Clients Initialize Finish\n";
}

int main()
{
	std::wcout.imbue(std::locale("korean"));

	for (auto& cl : g_clients)
		cl.is_active = false;

	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 0), &WSAData);
	h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	g_lSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	CreateIoCompletionPort((HANDLE)g_lSocket, h_iocp, KEY_SERVER, 0);
	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	::bind(g_lSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
	listen(g_lSocket, 5);
	SOCKET cSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	g_accept_over.op_mode = OP_MODE_ACCEPT;
	g_accept_over.wsa_buf.len = static_cast<ULONG>(cSocket);
	ZeroMemory(&g_accept_over.wsa_over, sizeof(g_accept_over.wsa_over));
	AcceptEx(g_lSocket, cSocket, g_accept_over.iocp_buf, 0, 32, 32, NULL, &g_accept_over.wsa_over);

	initialize_NPC();
	initialize_clients();

	thread timer_thread{ time_worker };
	thread db_thread{ db_worker };
	vector<thread> worker_threads;
	for (int i = 0; i < 6; ++i)
		worker_threads.emplace_back(worker_thread);
	for (auto& th : worker_threads)
		th.join();
	timer_thread.join();
	db_thread.join();

	closesocket(g_lSocket);
	WSACleanup();
}

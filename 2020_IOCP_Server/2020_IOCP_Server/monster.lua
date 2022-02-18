myid = 99999;
type = 0;
state = 0;
a_x = 0;
a_y = 0;
is_fight = false;
N_player = -1;

function init(id, t, s, x, y)
	myid = id;
	type = t;
	state = s;
	a_x = x;
	a_y = y;
end

function set_state(x)
	state = x;
end

function damaged(player)
	if (false == is_fight) then
		is_fight = true;
		N_player = player;
		state = 1;
		if(type == 0) then
			API_addTimer(myid, 9, 1);
		end
	end
end

function peace_fix()
	if (state == 0) then
		
	elseif (state == 1) then
		p_x = API_get_x(N_player);
		p_y = API_get_y(N_player);
		my_x = API_get_x(myid);
		my_y = API_get_y(myid);
		d_x = my_x - p_x;
		d_y = my_y - p_y;
		dist = d_x * d_x + d_y * d_y;
		if (dist < 2) then
			set_state(2);
			API_addTimer(myid, 9, 0);	
		else
			is_fight = false;
			set_state(0);
			N_player = -1;
			API_addTimer(myid, 9, 0);
		end
	else
		my_x = API_get_x(myid);
		my_y = API_get_y(myid);
		p_x = API_get_x(N_player);
		p_y = API_get_y(N_player);
		d_x = my_x - p_x;
		d_y = my_y - p_y;
		dist = d_x * d_x + d_y * d_y;
		if (dist < 2) then
			API_attack_player(myid, N_player);
		else
			set_state(1);
			API_addTimer(myid, 9, 0);
		end
	end
end

function peace_roaming()
	if (state == 0) then
		API_random_move(myid, a_x, a_y, 10);
	elseif (state == 1) then
		p_x = API_get_x(N_player);
		p_y = API_get_y(N_player);
		if ((a_x - 10) < p_x and p_x < (a_x + 10) and (a_y - 10) < p_y and p_y < (a_y + 10)) then
			API_npc_move_to_player(myid, N_player);
			my_x = API_get_x(myid);
			my_y = API_get_y(myid);
			d_x = my_x - p_x;
			d_y = my_y - p_y;
			dist = d_x * d_x + d_y * d_y;
			if (dist < 2) then
				set_state(2);
			end
		else
			is_fight = false;
			set_state(0);
			N_player = -1
			API_addTimer(myid, 9, 0);
		end
	else
		my_x = API_get_x(myid);
		my_y = API_get_y(myid);
		p_x = API_get_x(N_player);
		p_y = API_get_y(N_player);
		d_x = my_x - p_x;
		d_y = my_y - p_y;
		dist = d_x * d_x + d_y * d_y;
		if (dist < 2) then
			API_attack_player(myid, N_player);
		else
			set_state(1);
			API_addTimer(myid, 9, 0);
		end
	end
end

function aggro()
	if (state == 0) then	
		API_random_move(myid, a_x, a_y, 5);
		N_player = API_look_around(myid);
		if (-1 ~= N_player) then
			set_state(1);
		end
	elseif (state == 1) then
		p_x = API_get_x(N_player);
		p_y = API_get_y(N_player);
		if ((a_x - 5) < p_x and p_x < (a_x + 5) and (a_y - 5) < p_y and p_y < (a_y + 5)) then
			API_npc_move_to_player(myid, N_player);
			my_x = API_get_x(myid);
			my_y = API_get_y(myid);
			d_x = my_x - p_x;
			d_y = my_y - p_y;
			dist = d_x * d_x + d_y * d_y;
			if (dist < 2) then
				set_state(2);
			end
		else
			is_fight = false;
			set_state(0);
			N_player = -1;
			API_addTimer(myid, 9, 0);
		end
	else
		my_x = API_get_x(myid);
		my_y = API_get_y(myid);
		p_x = API_get_x(N_player);
		p_y = API_get_y(N_player);
		d_x = my_x - p_x;
		d_y = my_y - p_y;
		dist = d_x * d_x + d_y * d_y;
		if (dist < 2) then
			API_attack_player(myid, N_player);
		else
			set_state(1);
			API_addTimer(myid, 9, 0);
		end
	end
end

function act()
	if (type == 0) then
		peace_fix();
	elseif (type == 1) then
		peace_roaming();
	elseif (type == 2) then
		aggro();
	end
end
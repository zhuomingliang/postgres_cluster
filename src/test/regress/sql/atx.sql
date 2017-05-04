--- plain ---

create table atx_test(a int);
insert into atx_test values (1);

begin;
	update atx_test set a = 2;
	begin autonomous;
		update atx_test set a = 3;
	commit;
commit;

select * from atx_test; -- you should see (2)

delete from atx_test;
begin;
	insert into atx_test values (1);
	begin autonomous;
		insert into atx_test values (2);
		begin autonomous;
			insert into atx_test values (3);
			begin autonomous;
				insert into atx_test values (4);
				begin autonomous;
					insert into atx_test values (5);
					begin autonomous;
						insert into atx_test values (6);
						begin autonomous;
							insert into atx_test values (7);
						rollback;
					commit;
				rollback;
			commit;
		rollback;
	commit;
rollback;

select * from atx_test; -- you should see (2),(4),(6)

begin transaction isolation level repeatable read;
	begin autonomous transaction isolation level repeatable read;
		select 1;
	commit;
commit;

--- plpgsql ---

create or replace language plpgsql;

create or replace function myatx(x int) returns integer as $$
begin autonomous
	insert into atx_test values (123);
	begin autonomous
		insert into atx_test values (124);
	end;
	begin autonomous
		insert into atx_test values (125);
		raise exception 'hello world';
	end;
	insert into atx_test values (126);
	return x + 1;
end;
$$ language plpgsql;

select myatx(2000);

select * from atx_test; -- you should see (124)

--- audit ---

create table atx_actions (
	tid serial,
	table_name text,
	user_name text,
	tstamp timestamp with time zone default current_timestamp,
	action text,
	old_data text,
	new_data text,
	query text
);

create or replace function if_modified_func() returns trigger as $body$
declare
	v_old_data text;
	v_new_data text;
begin
	if (tg_op = 'UPDATE') then
		v_old_data := row(old.*);
		v_new_data := row(new.*);
		begin autonomous
			insert
				into atx_actions
				(table_name, user_name, action, old_data, new_data, query)
				values
				(tg_table_name::text, session_user::text, tg_op, v_old_data, v_new_data, current_query());
			return new;
		end;
	elsif (tg_op = 'DELETE') then
		v_old_data := row(old.*);
		begin autonomous
			insert
				into atx_actions
				(table_name, user_name, action, old_data, query)
				values
				(tg_table_name::text, session_user::text, tg_op, v_old_data, current_query());
			return old;
		end;
	elsif (tg_op = 'INSERT') then
		v_new_data := row(new.*);
		begin autonomous
			insert
				into atx_actions
				(table_name, user_name, action, new_data, query)
				values
				(tg_table_name::text, session_user::text, tg_op, v_new_data, current_query());
			return new;
		end;
	else
		raise warning 'if_modified_func - unhandled action %', tg_op;
		return null;
	end if;
end;
$body$ language plpgsql;

drop table atx_test;
create table atx_test(a text, b text);

create trigger atx_test_audit
after insert or update or delete on atx_test 
for each row execute procedure if_modified_func();

insert into atx_test values ('asd', 'bsd');
insert into atx_test values ('hello', 'world');
begin;
	delete from atx_test where a = 'asd';
	update atx_test set a = 'goodbye' where a = 'hello';
	-- atx_actions will keep the actions we performed even though we roll them back
rollback;

select * from atx_test;
select tid, table_name, action, old_data, new_data, query from atx_actions;

drop table atx_test;
drop table atx_actions;
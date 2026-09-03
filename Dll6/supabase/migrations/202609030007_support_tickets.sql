create table if not exists public.axiom_support_tickets (
    id uuid primary key default gen_random_uuid(),
    ticket_number bigint generated always as identity unique,
    user_id uuid not null references public.axiom_users(id) on delete restrict,
    status text not null default 'waiting_admin'
        check (status in ('waiting_admin', 'waiting_user', 'closed')),
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now(),
    last_message_at timestamptz,
    closed_at timestamptz,
    closed_by_telegram_user_id bigint
);

create unique index if not exists axiom_support_one_open_ticket_per_user
    on public.axiom_support_tickets(user_id) where status <> 'closed';
create index if not exists axiom_support_ticket_queue
    on public.axiom_support_tickets(status, last_message_at desc nulls last, created_at);

create table if not exists public.axiom_support_messages (
    id uuid primary key default gen_random_uuid(),
    ticket_id uuid not null references public.axiom_support_tickets(id) on delete cascade,
    sender_role text not null check (sender_role in ('user', 'admin', 'system')),
    sender_telegram_user_id bigint not null,
    body text,
    content_type text not null default 'text'
        check (content_type in ('text', 'photo', 'document')),
    source_bot text not null check (source_bot in ('axiom', 'support')),
    source_chat_id bigint not null,
    source_message_id bigint not null,
    source_file_id text,
    file_name text,
    mime_type text,
    delivery_status text not null default 'pending'
        check (delivery_status in ('pending', 'delivered', 'failed')),
    delivery_error text,
    created_at timestamptz not null default now(),
    delivered_at timestamptz,
    unique (source_bot, source_chat_id, source_message_id)
);

create index if not exists axiom_support_messages_ticket_history
    on public.axiom_support_messages(ticket_id, created_at);

create table if not exists public.axiom_support_user_sessions (
    user_id uuid primary key references public.axiom_users(id) on delete cascade,
    active_ticket_id uuid not null references public.axiom_support_tickets(id) on delete cascade,
    updated_at timestamptz not null default now()
);

create table if not exists public.axiom_support_admin_sessions (
    admin_telegram_user_id bigint primary key check (admin_telegram_user_id > 0),
    active_ticket_id uuid not null references public.axiom_support_tickets(id) on delete cascade,
    updated_at timestamptz not null default now()
);

alter table public.axiom_support_tickets enable row level security;
alter table public.axiom_support_messages enable row level security;
alter table public.axiom_support_user_sessions enable row level security;
alter table public.axiom_support_admin_sessions enable row level security;

revoke all on public.axiom_support_tickets,
    public.axiom_support_messages,
    public.axiom_support_user_sessions,
    public.axiom_support_admin_sessions from public, anon, authenticated;
grant select, insert, update, delete on public.axiom_support_tickets,
    public.axiom_support_messages,
    public.axiom_support_user_sessions,
    public.axiom_support_admin_sessions to service_role;
grant usage, select on sequence public.axiom_support_tickets_ticket_number_seq to service_role;

create or replace function public.axiom_support_open_ticket(
    p_telegram_user_id bigint
) returns jsonb
language plpgsql
security definer
set search_path = public
as $$
declare
    v_user public.axiom_users%rowtype;
    v_ticket public.axiom_support_tickets%rowtype;
    v_created boolean := false;
begin
    if p_telegram_user_id <= 0 then raise exception 'invalid Telegram user id'; end if;
    perform pg_advisory_xact_lock(p_telegram_user_id);

    select * into v_user from public.axiom_users
      where telegram_user_id = p_telegram_user_id for update;
    if not found then raise exception 'Telegram user not found'; end if;

    select * into v_ticket from public.axiom_support_tickets
      where user_id = v_user.id and status <> 'closed'
      order by created_at desc limit 1 for update;

    if not found then
        insert into public.axiom_support_tickets(user_id)
        values (v_user.id) returning * into v_ticket;
        v_created := true;
    end if;

    insert into public.axiom_support_user_sessions(user_id, active_ticket_id, updated_at)
    values (v_user.id, v_ticket.id, now())
    on conflict (user_id) do update set
        active_ticket_id = excluded.active_ticket_id,
        updated_at = excluded.updated_at;

    return jsonb_build_object(
        'id', v_ticket.id,
        'ticket_number', v_ticket.ticket_number,
        'status', v_ticket.status,
        'created', v_created
    );
end;
$$;

create or replace function public.axiom_support_close_ticket(
    p_ticket_id uuid,
    p_closed_by_telegram_user_id bigint
) returns jsonb
language plpgsql
security definer
set search_path = public
as $$
declare
    v_ticket public.axiom_support_tickets%rowtype;
begin
    select * into v_ticket from public.axiom_support_tickets
      where id = p_ticket_id for update;
    if not found then return jsonb_build_object('closed', false, 'reason', 'not_found'); end if;

    if v_ticket.status <> 'closed' then
        update public.axiom_support_tickets set
            status = 'closed', updated_at = now(), closed_at = now(),
            closed_by_telegram_user_id = p_closed_by_telegram_user_id
          where id = p_ticket_id;
    end if;
    delete from public.axiom_support_user_sessions where active_ticket_id = p_ticket_id;
    delete from public.axiom_support_admin_sessions where active_ticket_id = p_ticket_id;
    return jsonb_build_object('closed', true, 'idempotent', v_ticket.status = 'closed');
end;
$$;

revoke all on function public.axiom_support_open_ticket(bigint),
    public.axiom_support_close_ticket(uuid,bigint) from public, anon, authenticated;
grant execute on function public.axiom_support_open_ticket(bigint),
    public.axiom_support_close_ticket(uuid,bigint) to service_role;

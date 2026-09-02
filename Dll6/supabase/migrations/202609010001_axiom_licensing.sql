create extension if not exists pgcrypto;

create table if not exists public.axiom_licenses (
    id uuid primary key default gen_random_uuid(),
    key_hash text not null unique check (key_hash ~ '^[0-9a-f]{64}$'),
    label text not null default '',
    enabled boolean not null default true,
    expires_at timestamptz,
    max_devices integer not null default 1 check (max_devices between 1 and 20),
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

create table if not exists public.axiom_license_devices (
    license_id uuid not null references public.axiom_licenses(id) on delete cascade,
    device_hash text not null check (device_hash ~ '^[0-9a-f]{64}$'),
    first_seen_at timestamptz not null default now(),
    last_seen_at timestamptz not null default now(),
    revoked_at timestamptz,
    primary key (license_id, device_hash)
);

create table if not exists public.axiom_license_nonces (
    nonce_hash text primary key check (nonce_hash ~ '^[0-9a-f]{64}$'),
    created_at timestamptz not null default now()
);

create table if not exists public.axiom_license_attempts (
    id bigint generated always as identity primary key,
    ip_hash text not null check (ip_hash ~ '^[0-9a-f]{64}$'),
    device_hash text not null check (device_hash ~ '^[0-9a-f]{64}$'),
    accepted boolean not null default false,
    created_at timestamptz not null default now()
);

create index if not exists axiom_attempts_ip_created_idx
    on public.axiom_license_attempts (ip_hash, created_at desc);
create index if not exists axiom_attempts_device_created_idx
    on public.axiom_license_attempts (device_hash, created_at desc);
create index if not exists axiom_nonces_created_idx
    on public.axiom_license_nonces (created_at);

create table if not exists public.axiom_releases (
    id uuid primary key default gen_random_uuid(),
    product text not null default 'axiom',
    version text not null,
    storage_bucket text not null default 'axiom-modules',
    storage_path text not null,
    sha256 text not null check (sha256 ~ '^[0-9a-f]{64}$'),
    byte_size bigint not null check (byte_size between 4096 and 33554432),
    minimum_launcher_version text not null default '1.0.0',
    active boolean not null default false,
    created_at timestamptz not null default now(),
    unique (product, version)
);

create unique index if not exists axiom_one_active_release_idx
    on public.axiom_releases (product) where active;

alter table public.axiom_licenses enable row level security;
alter table public.axiom_license_devices enable row level security;
alter table public.axiom_license_nonces enable row level security;
alter table public.axiom_license_attempts enable row level security;
alter table public.axiom_releases enable row level security;

revoke all on public.axiom_licenses from anon, authenticated;
revoke all on public.axiom_license_devices from anon, authenticated;
revoke all on public.axiom_license_nonces from anon, authenticated;
revoke all on public.axiom_license_attempts from anon, authenticated;
revoke all on public.axiom_releases from anon, authenticated;
revoke all on sequence public.axiom_license_attempts_id_seq from anon, authenticated;
grant select, insert, update, delete on public.axiom_licenses to service_role;
grant select, insert, update, delete on public.axiom_license_devices to service_role;
grant select, insert, update, delete on public.axiom_license_nonces to service_role;
grant select, insert, update, delete on public.axiom_license_attempts to service_role;
grant select, insert, update, delete on public.axiom_releases to service_role;
grant usage, select on sequence public.axiom_license_attempts_id_seq to service_role;

insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
values ('axiom-modules', 'axiom-modules', false, 33554432,
        array['application/octet-stream', 'application/x-msdownload']::text[])
on conflict (id) do update set
    public = false,
    file_size_limit = excluded.file_size_limit,
    allowed_mime_types = excluded.allowed_mime_types;

create or replace function public.axiom_verify_and_bind_license(
    p_key_hash text,
    p_device_hash text,
    p_nonce_hash text,
    p_ip_hash text
) returns jsonb
language plpgsql
security definer
set search_path = public, pg_temp
as $$
declare
    v_license public.axiom_licenses%rowtype;
    v_attempt_id bigint;
    v_device_count integer;
    v_release jsonb;
begin
    if p_key_hash !~ '^[0-9a-f]{64}$'
       or p_device_hash !~ '^[0-9a-f]{64}$'
       or p_nonce_hash !~ '^[0-9a-f]{64}$'
       or p_ip_hash !~ '^[0-9a-f]{64}$' then
        return jsonb_build_object('ok', false, 'reason', 'invalid_request');
    end if;

    -- Serialize attempts for one device and make the device/IP limits deterministic.
    perform pg_advisory_xact_lock(hashtextextended(p_device_hash, 0));
    if (select count(*) from public.axiom_license_attempts
        where created_at > now() - interval '1 minute'
          and (ip_hash = p_ip_hash or device_hash = p_device_hash)) >= 15 then
        return jsonb_build_object('ok', false, 'reason', 'rate_limited');
    end if;

    insert into public.axiom_license_attempts(ip_hash, device_hash)
    values (p_ip_hash, p_device_hash) returning id into v_attempt_id;

    insert into public.axiom_license_nonces(nonce_hash) values (p_nonce_hash)
    on conflict do nothing;
    if not found then
        return jsonb_build_object('ok', false, 'reason', 'replayed_request');
    end if;

    perform pg_advisory_xact_lock(hashtextextended(p_key_hash, 1));
    select * into v_license from public.axiom_licenses
      where key_hash = p_key_hash for update;
    if not found or not v_license.enabled
       or (v_license.expires_at is not null and v_license.expires_at <= now()) then
        return jsonb_build_object('ok', false, 'reason', 'invalid_license');
    end if;

    if exists (select 1 from public.axiom_license_devices
               where license_id = v_license.id and device_hash = p_device_hash
                 and revoked_at is not null) then
        return jsonb_build_object('ok', false, 'reason', 'device_revoked');
    end if;

    if not exists (select 1 from public.axiom_license_devices
                   where license_id = v_license.id and device_hash = p_device_hash) then
        select count(*) into v_device_count from public.axiom_license_devices
          where license_id = v_license.id and revoked_at is null;
        if v_device_count >= v_license.max_devices then
            return jsonb_build_object('ok', false, 'reason', 'device_limit');
        end if;
        insert into public.axiom_license_devices(license_id, device_hash)
        values (v_license.id, p_device_hash);
    else
        update public.axiom_license_devices set last_seen_at = now()
          where license_id = v_license.id and device_hash = p_device_hash;
    end if;

    update public.axiom_license_attempts set accepted = true where id = v_attempt_id;
    update public.axiom_licenses set updated_at = now() where id = v_license.id;
    select jsonb_build_object(
        'version', version,
        'storage_bucket', storage_bucket,
        'storage_path', storage_path,
        'sha256', sha256,
        'byte_size', byte_size,
        'minimum_launcher_version', minimum_launcher_version
    ) into v_release from public.axiom_releases
      where product = 'axiom' and active limit 1;
    if v_release is null then
        return jsonb_build_object('ok', false, 'reason', 'no_active_release');
    end if;
    return jsonb_build_object(
        'ok', true,
        'license_id', v_license.id::text,
        'release', v_release
    );
end;
$$;

revoke all on function public.axiom_verify_and_bind_license(text,text,text,text)
    from public, anon, authenticated;
grant execute on function public.axiom_verify_and_bind_license(text,text,text,text)
    to service_role;

create or replace function public.axiom_prune_license_security_data()
returns void language sql security definer set search_path = public, pg_temp as $$
    delete from public.axiom_license_nonces where created_at < now() - interval '1 day';
    delete from public.axiom_license_attempts where created_at < now() - interval '30 days';
$$;
revoke all on function public.axiom_prune_license_security_data()
    from public, anon, authenticated;
grant execute on function public.axiom_prune_license_security_data() to service_role;

create or replace function public.axiom_publish_release(
    p_version text,
    p_storage_path text,
    p_sha256 text,
    p_byte_size bigint,
    p_minimum_launcher_version text default '1.0.0'
) returns uuid
language plpgsql security definer set search_path = public, pg_temp as $$
declare v_id uuid;
begin
    if p_version !~ '^\d{1,5}\.\d{1,5}\.\d{1,5}$'
       or p_minimum_launcher_version !~ '^\d{1,5}\.\d{1,5}\.\d{1,5}$'
       or p_sha256 !~ '^[0-9a-f]{64}$'
       or p_byte_size < 4096 or p_byte_size > 33554432
       or p_storage_path = '' or p_storage_path like '../%' then
        raise exception 'invalid release metadata';
    end if;
    update public.axiom_releases set active = false where product = 'axiom' and active;
    insert into public.axiom_releases(
        product, version, storage_bucket, storage_path, sha256, byte_size,
        minimum_launcher_version, active
    ) values (
        'axiom', p_version, 'axiom-modules', p_storage_path, p_sha256,
        p_byte_size, p_minimum_launcher_version, true
    ) on conflict (product, version) do update set
        storage_path = excluded.storage_path,
        sha256 = excluded.sha256,
        byte_size = excluded.byte_size,
        minimum_launcher_version = excluded.minimum_launcher_version,
        active = true
    returning id into v_id;
    return v_id;
end;
$$;
revoke all on function public.axiom_publish_release(text,text,text,bigint,text)
    from public, anon, authenticated;
grant execute on function public.axiom_publish_release(text,text,text,bigint,text)
    to service_role;

create or replace function public.axiom_authorize_download(
    p_license_id uuid,
    p_device_hash text
) returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare v_release jsonb;
begin
    if p_device_hash !~ '^[0-9a-f]{64}$' then return null; end if;
    if not exists (
        select 1 from public.axiom_licenses l
        join public.axiom_license_devices d on d.license_id = l.id
        where l.id = p_license_id and l.enabled
          and (l.expires_at is null or l.expires_at > now())
          and d.device_hash = p_device_hash and d.revoked_at is null
    ) then return null; end if;
    select jsonb_build_object(
        'storage_bucket', storage_bucket,
        'storage_path', storage_path,
        'sha256', sha256,
        'byte_size', byte_size
    ) into v_release from public.axiom_releases
      where product = 'axiom' and active limit 1;
    return v_release;
end;
$$;
revoke all on function public.axiom_authorize_download(uuid,text)
    from public, anon, authenticated;
grant execute on function public.axiom_authorize_download(uuid,text) to service_role;

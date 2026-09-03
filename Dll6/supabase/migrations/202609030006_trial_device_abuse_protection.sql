-- Trial abuse is enforced when a key is first verified by AxiomLauncher.
-- The launcher sends a local SHA-256 hardware fingerprint; the Edge Function
-- sends an HMAC of the source IP. Raw hardware identifiers and IPs are never
-- stored. A device can claim one trial forever, while an IP is only a secondary
-- velocity signal to avoid blocking ordinary shared networks.

create table if not exists public.axiom_trial_device_claims (
    device_hash text primary key check (device_hash ~ '^[0-9a-f]{64}$'),
    license_id uuid not null unique references public.axiom_licenses(id),
    user_id uuid not null unique references public.axiom_users(id),
    ip_hash text check (ip_hash is null or ip_hash ~ '^[0-9a-f]{64}$'),
    claimed_at timestamptz not null default now(),
    last_seen_at timestamptz not null default now()
);

create index if not exists axiom_trial_device_claims_ip_idx
    on public.axiom_trial_device_claims(ip_hash, claimed_at desc)
    where ip_hash is not null;

alter table public.axiom_trial_device_claims enable row level security;
revoke all on public.axiom_trial_device_claims from public, anon, authenticated;
grant select, insert, update, delete on public.axiom_trial_device_claims
    to service_role;

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
    v_trial_user_id uuid;
    v_existing_trial_license_id uuid;
    v_existing_trial_device_hash text;
begin
    if p_key_hash !~ '^[0-9a-f]{64}$'
       or p_device_hash !~ '^[0-9a-f]{64}$'
       or p_nonce_hash !~ '^[0-9a-f]{64}$'
       or p_ip_hash !~ '^[0-9a-f]{64}$' then
        return jsonb_build_object('ok', false, 'reason', 'invalid_request');
    end if;

    update public.axiom_trial_device_claims set ip_hash = null
      where claimed_at < now() - interval '90 days' and ip_hash is not null;

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

    -- A key remains trial-only until at least one paid activation/order extends it.
    select t.user_id into v_trial_user_id
      from public.axiom_trials t
     where t.license_id = v_license.id
       and not exists (
           select 1 from public.axiom_activation_codes a
            where a.license_id = v_license.id and a.status = 'redeemed')
       and not exists (
           select 1 from public.axiom_orders o
            where o.license_id = v_license.id and o.status = 'fulfilled');

    if v_trial_user_id is not null then
        select license_id into v_existing_trial_license_id
          from public.axiom_trial_device_claims
         where device_hash = p_device_hash;
        if found and v_existing_trial_license_id <> v_license.id then
            return jsonb_build_object('ok', false, 'reason', 'trial_device_used');
        end if;

        select device_hash into v_existing_trial_device_hash
          from public.axiom_trial_device_claims
         where license_id = v_license.id;
        if found and v_existing_trial_device_hash <> p_device_hash then
            return jsonb_build_object('ok', false, 'reason', 'trial_device_locked');
        end if;

        -- Serialize the secondary network check. Three different PCs per
        -- 30 days are allowed for shared households/NAT; further trial claims
        -- from the same HMAC'd IP are rejected.
        perform pg_advisory_xact_lock(hashtextextended(p_ip_hash, 2));
        if v_existing_trial_device_hash is null and (
            select count(*) from public.axiom_trial_device_claims
             where ip_hash = p_ip_hash
               and claimed_at > now() - interval '30 days') >= 3 then
            return jsonb_build_object('ok', false, 'reason', 'trial_network_limit');
        end if;

        insert into public.axiom_trial_device_claims(
            device_hash, license_id, user_id, ip_hash
        ) values (
            p_device_hash, v_license.id, v_trial_user_id, p_ip_hash
        ) on conflict (device_hash) do update set last_seen_at = now();
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
    update public.axiom_trial_device_claims set ip_hash = null
      where claimed_at < now() - interval '90 days' and ip_hash is not null;
$$;
revoke all on function public.axiom_prune_license_security_data()
    from public, anon, authenticated;
grant execute on function public.axiom_prune_license_security_data()
    to service_role;

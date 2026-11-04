drop policy if exists "Allow anonymous secure telemetry broadcasts"
  on realtime.messages;

create policy "Allow anonymous secure telemetry broadcasts"
  on realtime.messages
  for select
  to anon
  using (extension = 'broadcast' and topic = 'secure-telemetry');

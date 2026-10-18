# Playing TLOU2 Co-op

Two PCs, each with its own copy of *The Last of Us Part II Remastered*. One PC is the **host**: its story is the
one you both play. The other is the **partner**. On each PC, your partner shows up as a second player in their
character's look.

## Both PCs: install once

1. Put the mod files in a `tlou2-coop` folder next to `tlou-ii.exe` (`deploy.cmd` does this).
2. In `tlou2-coop\coop.ini`, under `[session]`:
   - host PC: `Mode = host`
   - partner PC: `Mode = client`
3. Play in borderless or windowed mode.

## Playing on the same home network (LAN)

1. Start the game on both PCs: `tlou-ii.exe -nolauncher`, not as administrator.
2. Run `tlou2-coop\coop_launcher.exe` on both PCs (before or after the game; it attaches by itself).
3. Load your save on the host. The partner finds the host on its own and joins.
4. If the partner can't find the host:
   - set `HostAddress` in the partner's `coop.ini` to the host PC's IP address (for example `192.168.0.237`);
   - allow the game through Windows Firewall on the host (UDP port 47820);
   - with a VPN such as Mullvad, turn on its "Local network sharing" option.

## Playing over the internet (relay)

The relay is a small program that passes the game traffic between the two PCs. Run it on a machine that both
of you can reach: a cheap cloud server, or one of your own PCs with a port forwarded on the router.

1. Pick a secret: any phrase of 8 or more characters. Both PCs and the relay use the same one.
2. On the relay machine, run:

   ```
   coop_relay.exe your-secret-here
   ```

   It listens on UDP port **47830**. If that machine is behind a home router, forward UDP 47830 to it, and allow
   `coop_relay.exe` through its firewall. Every 10 s it prints whether the host is connected and how many
   partners there are.
3. In both PCs' `coop.ini`, under `[session]`:

   ```
   RelayAddress = <the relay machine's public IP address>
   RelayPort = 47830
   RelaySecret = your-secret-here
   ```

4. Start the games and `coop_launcher.exe` as usual. The host's log says `hosting through relay`, and the
   partner's says `relay: joining through`.
5. To go back to LAN play, empty `RelayAddress` and `RelaySecret` again.

Neither PC needs a port forwarded: both only connect out to the relay. Anyone without the secret is ignored.

Switching without editing `coop.ini` (for a quick test): add a line `relay <ip> 47830 <secret>` to
`tlou2-coop\cmd.txt` on each PC, and `relay off` to go back to LAN.

## Good to know

- The partner's game updates the mod by itself from the host (LAN only).
- In cutscenes and scripted moments the real story companion takes over, then your partner continues from
  where she ended up.
- If you fall far behind, hold T (or Back on a controller) to teleport to the other player.

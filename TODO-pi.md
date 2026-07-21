# TODO: Umbenennung sporegeist → geist-agent auf dem Pi nachziehen

Repo wurde in `geist-agent` umbenannt (Commit 9ab3879); Binaries, Lib, Includes
und Env-Variablen heißen jetzt `geist-agent`/`GEIST_AGENT_*`.

- [ ] Remote-URL aktualisieren: `git remote set-url origin https://github.com/geisten/geist-agent.git` (Redirect funktioniert, explizit ist besser)
- [ ] `git pull`
- [ ] Env-Variablen umbenennen: `SPOREGEIST_API_KEY` → `GEIST_AGENT_API_KEY`, `SPOREGEIST_API_URL` → `GEIST_AGENT_API_URL`
      Fundstellen suchen: `grep -rl SPOREGEIST ~/.profile ~/.bashrc /etc/systemd/system /etc/environment 2>/dev/null`
- [ ] systemd-Units / Autostart: Binary-Pfade auf `bin/geist-agent` bzw. `bin/geist-agent-chat` umstellen, danach `systemctl daemon-reload`
- [ ] Neu bauen: `make clean && make && make test`
- [ ] Alte Binaries/Symlinks entfernen: `which sporegeist sporegeist-chat`
- [ ] Eigene Skripte/Cron-Jobs prüfen: `grep -r sporegeist ~/bin crontab -l`

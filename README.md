# mfm – minimaler Dateibrowser für X11 (passend zu mwm)

Eine C-Datei, nur Xlib + Xft, kein Toolkit. Motif-Look mit denselben Farben
wie [mwm-x11](https://github.com/reygnn/mwm-x11): `MWM_INACTIVE` / `MWM_ACTIVE`
aus der Umgebung, sonst `Mwm*background` / `Mwm*activeBackground` aus
`~/.config/mwm/mwmrc`.

## Bauen

    sudo apt install build-essential pkg-config libx11-dev libxft-dev
    make
    sudo make install        # -> /usr/local/bin/mfm

Zum Öffnen von Dateien optional: `sudo apt install xdg-utils`.

## Starten

    mfm [verzeichnis | datei]

Mit einer Datei öffnet mfm deren Ordner und wählt sie aus. Ohne Argument
startet er im aktuellen Verzeichnis. Lässt sich das Startverzeichnis nicht
öffnen, zeigt er `$HOME` und meldet den Grund in der Statuszeile.

Beim Start von mwm, z. B. in `~/.config/mwm/autostart`:

    #!/bin/sh
    mfm "$HOME" &

## Tasten

| Taste | Aktion |
|---|---|
| ↑ ↓ Bild↑ Bild↓ Pos1 Ende (auch Ziffernblock) | bewegen |
| Enter / → | Ordner öffnen bzw. Datei öffnen |
| Backspace / ← / Alt+↑ | übergeordneter Ordner |
| Alt+Pos1 | Home-Verzeichnis |
| Buchstaben tippen | zum Namen springen (Groß/klein egal, auch bei Umlauten; nach 1 s beginnt die Suche neu) |
| Ctrl+L | Pfad eingeben (`/…` oder `~…` beginnt neu, Ctrl+U leert) |
| Ctrl+H | versteckte Dateien ein/aus |
| Ctrl+R / F5 | neu laden |
| Ctrl+T | Terminal im aktuellen Ordner |
| Ctrl+Q / Ctrl+W / Alt+F4 | beenden (Alt+F4 fängt unter mwm der Window Manager ab und schließt das Fenster) |
| Leertaste / Einfg | markieren bzw. Markierung aufheben, weiter zur nächsten Zeile (Leertaste während einer Namenssuche gehört zum Namen) |
| Ctrl+A | alles markieren bzw. alle Markierungen aufheben |
| Ctrl+C / Ctrl+X | Markierte (sonst die Auswahl) zum Kopieren bzw. Verschieben merken |
| Ctrl+V | im aktuellen Ordner einfügen |
| Entf | in den Papierkorb |
| Shift+Entf | endgültig löschen, nach Rückfrage (`y`/`j` bestätigt, jede andere Taste bricht ab) |
| Esc | Suche/Meldung löschen, ohne laufende Suche: Markierungen aufheben |

Maus: Klick wählt, Doppelklick öffnet, Mausrad scrollt, Scrollbar ziehen
oder in die Leiste klicken, Pfeil-Button oder Maus-Zurück-Taste (8) für
„nach oben“, Klick in die Pfadzeile zum Bearbeiten, Ctrl+Klick markiert,
Shift+Klick markiert den Bereich bis zur Auswahl.

## Kopieren, Verschieben, Löschen

Die Markierungen gelten im aktuellen Ordner und bleiben beim automatischen
Neuladen erhalten; beim Ordnerwechsel verfallen sie. Ohne Markierung
wirkt jede Aktion auf die ausgewählte Zeile.

Kopieren (`cp -a`), Verschieben und endgültiges Löschen (`rm -rf`) laufen
in einem Hintergrundprozess, das Fenster bleibt bedienbar; eine weitere
Aktion wartet, bis die laufende fertig ist. Verschoben wird per
`rename`, über Dateisystemgrenzen mit `mv`. Das Ergebnis, bei Fehlern der
erste davon, steht in der Statuszeile.

Überschrieben wird nie: Ist ein Name im Ziel schon vergeben, heißt die
Kopie `name (copy).txt`, `name (copy 2).txt` usw. So erzeugt auch
Ctrl+C, Ctrl+V im selben Ordner ein Duplikat. Einen Ordner in sich selbst
zu kopieren oder zu verschieben lehnt mfm ab.

Der Papierkorb folgt der freedesktop.org-Spezifikation, Dateien lassen
sich also mit jedem Dateimanager wiederherstellen, der sie kennt (oder per
Hand aus `files/`, der Originalpfad steht in `info/*.trashinfo`). Er liegt
in `$XDG_DATA_HOME/Trash` (Standard `~/.local/share/Trash`), für Dateien
auf anderen Dateisystemen in `.Trash-$UID` an deren Wurzel. Geht das nicht
(z. B. schreibgeschützt), bleibt Shift+Entf.

Der Ordner wird jede Sekunde auf Änderungen geprüft und neu eingelesen,
wobei Auswahl und Scrollposition erhalten bleiben. Verschwindet er oder wird
er unlesbar, springt mfm zum nächsten lesbaren Elternordner.

Pfade sind logisch wie bei `cd` in der Shell: Nach dem Betreten eines
verlinkten Ordners führt `..` zurück in den Ordner, in dem der Link liegt,
und ein mit Ctrl+T geöffnetes Terminal bekommt diesen Pfad als `$PWD`.
Ein Elternordner, den man durchqueren, aber nicht auflisten darf, wird
bei Backspace übersprungen.

Texteingabe läuft über das in `XMODIFIERS` eingestellte Eingabesystem
(ibus, fcitx, …). Läuft es nicht, nimmt mfm das in Xlib eingebaute (mit
Compose und toten Tasten) und wechselt, sobald das eigentliche startet.

Dateinamen, die kein gültiges UTF-8 sind (z. B. Latin-1 von alten Platten
oder Netzlaufwerken), werden als Latin-1 angezeigt.

## Umgebung

| Variable | Standard | |
|---|---|---|
| `MFM_OPEN` | `xdg-open` | Programm zum Öffnen von Dateien, z. B. `feh` oder `"mimeopen -n"`; die Datei wird als letztes Argument angehängt |
| `MFM_TERMINAL` | `$MWM_TERMINAL`, sonst das erste gefundene von xterm, uxterm, x-terminal-emulator, urxvt, alacritty, konsole, kitty, xfce4-terminal, gnome-terminal | für Ctrl+T |
| `MFM_FONT` | `Sans:size=10` | fontconfig-Muster |
| `MWM_INACTIVE`, `MWM_ACTIVE` | `#aeb2c3`, `#7387ad` | Hintergrund, Auswahl |

`MFM_OPEN` und `MFM_TERMINAL` werden an Leerzeichen in Wörter zerlegt.
Anführungszeichen fassen Wörter zusammen (`xterm -fa 'DejaVu Sans Mono'`),
sonst gibt es keine Shell-Syntax (keine `$VAR`, kein `;`, keine Pipes); für
so etwas ein kleines Skript eintragen. Das Programm wird nur in den
absoluten Einträgen von `$PATH` gesucht, nie im gerade angezeigten Ordner.
Fehlt es, steht das in der Statuszeile.

Nicht enthalten (bewusst): Umbenennen, Ordner anlegen. Dafür Ctrl+T und
die Shell.

## Lizenz

MIT, siehe [LICENSE](LICENSE).

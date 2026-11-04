# mfm – minimaler Dateibrowser für Wayland (passend zu mwc)

Eine C-Datei, nur libwayland, xkbcommon, Cairo und Pango, kein Toolkit.
Motif-Look mit denselben Farben wie [mwc](https://github.com/reygnn/mwc):
`MWC_INACTIVE` / `MWC_ACTIVE` aus der Umgebung, sonst `Mwm*background` /
`Mwm*activeBackground` aus `~/.config/mwc/mwmrc`.

Die Version für X11 ist [mfm-x11](https://github.com/reygnn/mfm-x11).

## Bauen

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libxkbcommon-dev libcairo2-dev libpango1.0-dev
    make
    sudo make install        # -> /usr/local/bin/mfm

Zum Öffnen von Dateien optional: `sudo apt install xdg-utils`.

## Starten

    mfm [verzeichnis | datei]

Mit einer Datei öffnet mfm deren Ordner und wählt sie aus. Ohne Argument
startet er im aktuellen Verzeichnis. Lässt sich das Startverzeichnis nicht
öffnen, zeigt er `$HOME` und meldet den Grund in der Statuszeile.

Beim Start von mwc, z. B. in `~/.config/mwc/autostart`:

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
| F2 | umbenennen (der Name ohne Endung ist ausgewählt: Tippen ersetzt ihn, ← → setzen den Cursor) |
| F7 / Ctrl+Shift+N | neuer Ordner |
| Ctrl+H | versteckte Dateien ein/aus |
| Ctrl+R / F5 | neu laden |
| Ctrl+T | Terminal im aktuellen Ordner |
| Ctrl+Q / Ctrl+W / Alt+F4 | beenden (Alt+F4 fängt unter mwc der Compositor ab und schließt das Fenster) |
| Leertaste / Einfg | markieren bzw. Markierung aufheben, weiter zur nächsten Zeile (Leertaste während einer Namenssuche gehört zum Namen) |
| Ctrl+A | alles markieren bzw. alle Markierungen aufheben |
| Ctrl+C / Ctrl+X | Markierte (sonst die Auswahl) zum Kopieren bzw. Verschieben merken |
| Ctrl+V | im aktuellen Ordner einfügen |
| Entf | in den Papierkorb |
| Shift+Entf | endgültig löschen, nach Rückfrage (`y`/`j` bestätigt, jede andere Taste bricht ab) |
| Esc | der Reihe nach: Suche löschen, sonst Meldung löschen, sonst Markierungen aufheben |

In Eingabefeldern (Pfad, Umbenennen, neuer Ordner): ← → Pos1 Ende bewegen
den Cursor, Backspace/Entf löschen, Ctrl+U leert, Enter bestätigt, Esc
bricht ab.

Maus: Klick wählt, Doppelklick öffnet, Mausrad scrollt, Scrollbar ziehen
oder in die Leiste klicken, Pfeil-Button oder Maus-Zurück-Taste (8) für
„nach oben“, Klick in die Pfadzeile zum Bearbeiten, Ctrl+Klick markiert,
Shift+Klick markiert den Bereich bis zur Auswahl, ein einfacher Klick hebt
alle Markierungen auf.

Rechtsklick in die Liste öffnet ein Kontextmenü mit den Befehlen von oben
(Öffnen, Ausschneiden, Kopieren, Einfügen, Umbenennen, Papierkorb,
endgültig löschen, neuer Ordner, Terminal, versteckte Dateien, neu laden).
Ein Rechtsklick auf einen markierten Eintrag behält die Markierungen:
Ausschneiden, Kopieren, Papierkorb und Löschen wirken dann auf alle
markierten, Öffnen und Umbenennen gibt es nur bei höchstens einer
Markierung. Ein Rechtsklick auf einen anderen Eintrag wählt nur diesen
aus. Auf freier Fläche wirkt das Menü auf die Markierungen, sonst nur auf
den Ordner. Solange kopiert oder verschoben wird, sind Einfügen,
Umbenennen, Papierkorb, Löschen und neuer Ordner gesperrt. Gedrückt
halten, auf einen Befehl ziehen und loslassen führt ihn aus, ebenso ein
Klick darauf; ↑ ↓ und Enter gehen auch, Esc oder ein Klick daneben
schließt das Menü. Hat sich die Liste geändert, während das Menü offen war
(Eintrag verschwunden, anderer Ordner), passiert nichts.

## Dateien verwalten

Die Markierungen gelten im aktuellen Ordner und bleiben beim automatischen
Neuladen erhalten; beim Ordnerwechsel, nach Ctrl+C/Ctrl+X und bei einem
einfachen Klick verfallen sie. Ohne Markierung wirkt jede Aktion auf die
ausgewählte Zeile.

Kopieren, Verschieben und endgültiges Löschen laufen in einem
Hintergrundprozess, das Fenster bleibt bedienbar; eine weitere Aktion
(auch F2/F7) wartet, bis die laufende fertig ist. Beendet man mfm
vorher, läuft sie zu Ende. Das Ergebnis, bei Fehlern der erste davon,
steht in der Statuszeile. Nach Ctrl+X bleibt die Zwischenablage stehen,
bis das Verschieben ganz geklappt hat. Stirbt der Hintergrundprozess, meldet
mfm das und lässt die Zwischenablage ebenfalls stehen.

Überschrieben wird nie. Verschoben wird innerhalb eines Dateisystems per
`rename`, das keinen vorhandenen Namen ersetzt. Kopien (`cp -a`) und
Verschiebungen über Dateisystemgrenzen (`mv`) landen zuerst in einem
versteckten Ordner `.mfm-XXXXXX` im Ziel und werden von dort umbenannt:
Eine halbe Kopie erscheint nie unter dem echten Namen, und der Name wird
erst in diesem Moment gewählt. Ist er vergeben (auch nur in anderer
Groß-/Kleinschreibung auf FAT-Laufwerken), heißt die Kopie
`name (copy).txt`, `name (copy 2).txt` usw., ein verschobener Eintrag
`name (2).txt`, `name (3).txt`. So erzeugt auch Ctrl+C, Ctrl+V im
selben Ordner ein Duplikat. Einen Ordner in sich selbst zu kopieren oder
zu verschieben lehnt mfm ab. Auch Umbenennen überschreibt nichts; ist der
neue Name vergeben, meldet mfm das. (Einzige Lücke: Dateisysteme ohne
atomares „umbenennen ohne zu ersetzen“, z. B. NFS, bei Ordnern, und bei
Dateien, wenn sie auch keine Hardlinks können.)

Scheitert ein Verschieben über Dateisystemgrenzen, z. B. weil das Ziel voll
ist, bleibt das Angekommene im versteckten `.mfm-XXXXXX`, und die Statuszeile
nennt den Ordner. Nach dem Fehlschlag ist die Quelle in aller Regel noch
vollständig. Eine gescheiterte Kopie räumt mfm weg.

Endgültiges Löschen ist `rm -rf --one-file-system`. Einen Einhängepunkt selbst
und einen Ordner, in dem etwas eingehängt ist, löscht mfm nicht, auch nicht
Bind-Mounts vom selben Dateisystem, deren Inhalt `rm` sonst leeren würde. Auch
verschieben lässt sich ein Einhängepunkt nicht, und über Dateisystemgrenzen auch
kein Ordner, in dem etwas eingehängt ist; kopieren geht.

Der Papierkorb folgt der freedesktop.org-Spezifikation, Dateien lassen
sich also mit jedem Dateimanager wiederherstellen, der sie kennt (oder per
Hand aus `files/`, der Originalpfad steht in `info/*.trashinfo`). Er liegt
in `$XDG_DATA_HOME/Trash` (Standard `~/.local/share/Trash`), für Dateien
auf anderen Dateisystemen an deren Wurzel in `.Trash/$UID` (wenn der
Administrator einen gemeinsamen `.Trash` eingerichtet hat), sonst in
`.Trash-$UID`; dort steht der Originalpfad relativ zur Wurzel, damit er
auch stimmt, wenn der Datenträger woanders eingehängt wird. Was schon im
Papierkorb liegt, wird nicht noch einmal hineingelegt. Geht es nicht
(z. B. schreibgeschützt), bleibt Shift+Entf.

Der Ordner wird jede Sekunde auf Änderungen geprüft und neu eingelesen,
wobei Auswahl und Scrollposition erhalten bleiben. Verschwindet er oder wird
er unlesbar, springt mfm zum nächsten lesbaren Elternordner. Geänderte Größen
oder Rechte einer Datei ändern den Ordner nicht: Die zeigt erst Ctrl+R.

Pfade sind logisch wie bei `cd` in der Shell: Nach dem Betreten eines
verlinkten Ordners führt `..` zurück in den Ordner, in dem der Link liegt,
und ein mit Ctrl+T geöffnetes Terminal bekommt diesen Pfad als `$PWD`.
Ein Elternordner, den man durchqueren, aber nicht auflisten darf, wird
bei Backspace übersprungen.

Texteingabe kann Compose und tote Tasten, nach den Regeln der Locale
(xkbcommon). Eingabesysteme wie ibus oder fcitx gehen nicht: Dafür bräuchte
es `text-input-v3`, das mwc nicht anbietet.

Dateinamen, die kein gültiges UTF-8 sind (z. B. Latin-1 von alten Platten
oder Netzlaufwerken), werden als Latin-1 angezeigt. Steuerzeichen für die
Schreibrichtung erscheinen als `�`, damit `zz\u202Efdp.exe` nicht wie
`zzexe.pdf` aussieht.

Gehalten wiederholen nur Bewegen und Tippen, keine Befehle: Entf, Enter und
Tastenkombinationen wirken einmal je Druck, Entf und Backspace in Eingabefeldern
wiederholen.

## Umgebung

| Variable | Standard | |
|---|---|---|
| `MFM_OPEN` | `xdg-open` | Programm zum Öffnen von Dateien, z. B. `feh` oder `"mimeopen -n"`; die Datei wird als letztes Argument angehängt |
| `MFM_TERMINAL` | `$MWC_TERMINAL`, sonst das erste gefundene von foot, xterm, uxterm, x-terminal-emulator, urxvt, alacritty, konsole, kitty, xfce4-terminal, gnome-terminal | für Ctrl+T |
| `MFM_FONT` | `Sans 10` | Pango-Name, z. B. `Sans Bold 11` (ohne Größe: 10); fontconfig-Muster wie in mfm-x11 (`Sans:size=10`) werden abgelehnt |
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aeb2c3`, `#7387ad` | Hintergrund, Auswahl |

`MFM_OPEN` und `MFM_TERMINAL` werden an Leerzeichen in Wörter zerlegt.
Anführungszeichen fassen Wörter zusammen (`xterm -fa 'DejaVu Sans Mono'`),
sonst gibt es keine Shell-Syntax (keine `$VAR`, kein `;`, keine Pipes); für
so etwas ein kleines Skript eintragen. Das Programm wird nur in den
absoluten Einträgen von `$PATH` gesucht, nie im gerade angezeigten Ordner.
Fehlt es, steht das in der Statuszeile.

Alles darüber hinaus (Rechte, Links, Archive, …): Ctrl+T und die Shell.

## Compositor

mfm braucht `xdg-shell` und fragt per `xdg-decoration` nach einem Rahmen vom
Compositor. Wer das nicht anbietet (etwa GNOME/Mutter), zeigt mfm ohne Rahmen.
Ein Fenstersymbol wie in mfm-x11 gibt es nicht: Wayland kennt `_NET_WM_ICON`
nicht, der Compositor sieht nur die `app_id` `mfm`.

Zeigerbewegungen mit gedrückter Taste brauchen vom Compositor einen impliziten
Grab: Der Zeiger gehört der Oberfläche, auf der die Taste gedrückt wurde.
Ohne ihn erfährt mfm nicht, wenn die Taste woanders losgelassen wird. Dann
endet das Ziehen der Scrollbar am Fensterrand, und beim Kontextmenü führt
„gedrückt halten, ziehen, loslassen“ den Befehl nicht aus: Das Menü bleibt
offen, ein Klick auf den Befehl geht weiter.

## Tests

    make test

fährt mfm unter einem headless laufenden mwc mit simulierter Maus und
Tastatur und prüft Dateien und Screenshots (`tests/ui.sh`), dazu Dateioperationen
mit echten Mounts in einem privaten Namespace (`tests/files.sh`; ohne User-Namespaces
übersprungen). Es braucht einen gebauten mwc samt `build/vinput` und
`build/pngtool` in `MWC_DIR` (sonst `../mwc` oder `../mwm`) sowie `grim`. Die Tests
haben einen eigenen Socket und fassen eine laufende Sitzung nicht an.

## Lizenz

MIT, siehe [LICENSE](LICENSE).

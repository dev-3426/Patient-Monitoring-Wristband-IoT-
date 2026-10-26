NEUROBAND FINAL CARETAKER DASHBOARD
=====================================

FILES
-----
index.html
style.css
app.js

HOW TO RUN
----------
1. Open this folder in VS Code.
2. Install the "Live Server" extension.
3. Right-click index.html.
4. Select "Open with Live Server".
5. Keep the ESP32 and Neuroband firmware running.

ALARM SOUND
-----------
Browsers block automatic sound until the user interacts with the page.
Click "Enable Alarm Sound" once after opening the dashboard.

MAP
---
The dashboard uses Leaflet with OpenStreetMap tiles.
It shows:
- configured safe-zone centre
- 5 metre safe-zone radius
- current patient marker when GPS is available

DEVICE ONLINE STATUS
--------------------
For reliable "Device Online / Offline" detection, upload the included
Neuroband V6.2 firmware. It writes:

/neuroband/system/uptimeMillis

every upload cycle. The dashboard marks the device offline when no fresh
heartbeat is received for 15 seconds.

FIREBASE RULES
--------------
The current open rules are suitable only for development testing.
Secure the database before public deployment.

# If you were given one

Part of [Plane Radar](../README.md).

A small round screen that shows the planes flying near your home, live. Plug it
into a USB charger, then set it up once from your phone. It takes a few minutes.

**Step 1 of 2: join the radar's Wi-Fi and pick yours.** The screen shows a QR
code. Scan it with the phone's camera. This joins the radar's own setup Wi-Fi,
`PlaneRadar-Setup`. A page opens and says what the radar is (by its name, if
it was given one). If it does not open, open `http://192.168.4.1`. Pick your
home Wi-Fi, type its password, tap **Save**. Only 2.4 GHz networks work; the
radar cannot use 5 GHz. No camera? Tap the button on the radar. It shows the
network name and its password in words.

If whoever gave it to you kept its location, the screen says **Wi-Fi setup**
instead of *Step 1 of 2*, and this is the only step.

**Step 2 of 2: tell it where you live.** Put the phone back on your home Wi-Fi.
Scan the next QR code on the radar's screen. (Or open the address the screen
shows, which ends in `/settings`. `http://plane-radar.local` works too.) Type a
town or ZIP code, paste coordinates from a map app, or type latitude and
longitude. Tap **Save location**.

**Then it says welcome.** The first time it is on your Wi-Fi and knows where it
is, the screen says *Welcome to* its name (or *your Plane Radar*) and shows a QR
code for a short guide: what is on the screen, what the button does, and
what to do if something looks wrong. Scan it with the phone on your home Wi-Fi.
A tap on the button skips it, and it goes by itself after 3 minutes. Then the
planes appear. The guide stays at `http://plane-radar.local/guide` (**Guide**
in the menu of every page).

## The button

The button is marked **BOOT**.

| What you do | What happens |
|-------------|--------------|
| **Tap** | Changes the range: 5, 10, 15, 25 km, then round again. On the welcome screen, skips it. |
| **Hold 2 to 9 seconds, then let go** | While the radar is showing, shows an info screen: a QR code and the address of the settings page, the version, and whether flight data is arriving. A tap closes it. |
| **Hold 10 seconds** | The radar forgets its Wi-Fi and goes back to step 1. Your location and settings are kept. A countdown shows from 2 seconds; let go before it ends and nothing is forgotten. |

## Changing Wi-Fi or location later

- **Location:** on your home Wi-Fi, open `http://plane-radar.local` (the info
  screen shows the address too). Go to **Settings**, type a new town, ZIP code
  or coordinates, tap **Save location**.
- **Wi-Fi:** hold the button for 10 seconds, then do step 1 again. Use this
  after a new router or a new Wi-Fi password.
- Everything else (theme, text size, night dimming, alerts) is on the same
  **Settings** page, at `http://plane-radar.local`.

## If something looks wrong

- **The screen says "Wi-Fi lost".** The radar keeps trying by itself, and
  restarts after 10 minutes. Unplug it and plug it back in. Check that the
  router is on and that the network is 2.4 GHz.
- **No planes, or "No flight data".** Unplug it and plug it back in. Check the
  Wi-Fi as above, and that the internet works. Tap the button for a wider
  range. Some skies are quiet. "Next update in 3 min" under it means the main
  flight data service is down and the radar is on its reserve, which allows
  only a few updates an hour: nothing to do, it goes back by itself.
- **It feels warm.** That is normal.

## Updates and giving it away

New versions arrive by themselves, about once a day. They keep your Wi-Fi,
location and settings. If a new version does not work, the radar goes back to
the old one by itself. You do not need to do anything. The **Settings** page
shows the version and has a **Check for an update now** button.

To give the radar to someone else, open **Settings** and tap **Reset for a new
owner**. It asks again, then erases the location, the name, the settings and
the Wi-Fi. Tick **Keep the location and the radar's name** if it stays in the
area: the next person then only joins it to their Wi-Fi. Give it a name first
if you like (**Settings**, *Name of this radar*): the setup page, the welcome
screen and the guide greet them by it, as in "Welcome to Ada's radar". Reset it
last: the welcome shows once, to whoever sets the radar up next. If you try it
out yourself afterwards, reset it again before you give it away.

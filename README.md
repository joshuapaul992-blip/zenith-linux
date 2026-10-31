# Donation Central (original build)

A from-scratch Roblox donation game inspired by the *Donation Central* style of
game — players set up a stand, link their **own** game passes, and other players
donate by buying those passes. It includes global leaderboards, rotating live
in-game events, and an owner dashboard.

**Everything here is original.** All code is written from scratch and every
visual is drawn in code from solid colours and gradients — there are **no
imported models, decals, or image asset IDs** anywhere in the project.

---

## How the "payout" actually works (read this first)

Roblox has **no API** that lets a script pay Robux out to players, and Robux you
earn from **Developer Products** lands in *your* balance (you'd then have to pay
people manually via Group Payouts, which is slow and only works for group
members).

This game sidesteps all of that the same way Donation Central does — with
**game passes**:

- Each player links **game passes they created and own**.
- When someone donates, they **buy that player's pass**.
- Roblox pays the **pass owner directly** (minus Roblox's ~30% marketplace fee).

So there is **nothing to pay out by hand** — every donation settles
automatically through Roblox. The game's job is to:

1. Verify a player actually owns a pass before they can link it (prevents
   leaderboard spoofing — the Robux always goes to the true owner regardless).
2. Record totals for the leaderboards and the owner dashboard.
3. Make donating fun with stands, events, and live feedback.

> ⚠️ **Game-pass limitation:** a player can only own a given pass *once*, so a
> buyer can't repeatedly buy the same pass. Players list **several passes at
> different prices** (e.g. 10, 50, 100, 500 R$) so donors have options. This is
> the same trade-off real donation games work within.

---

## Opening the file in Studio

Open **`DonationCentral.rbxlx`** directly:

- Roblox Studio → **File → Open from File…** → pick `DonationCentral.rbxlx`

`.rbxlx` is the XML form of a Roblox place file; Studio opens it exactly like a
binary `.rbxl`. (If you specifically want a binary `.rbxl`, see
[Rebuilding](#rebuilding-the-place-file) below.)

Then press **Play**. In Studio you'll also want:

- **Game Settings → Security → Enable Studio Access to API Services** (so the
  DataStore leaderboards/persistence work while testing).

---

## First-time setup

1. **Publish** the place to Roblox (File → Publish to Roblox).
2. Open `ReplicatedStorage → DonationShared → Config` and set:
   - `OwnerUserIds` — your Roblox userId, to unlock the **Dashboard** tab.
     (The experience creator is always an owner automatically.)
   - `AllowedGroupId` — only if you want to allow **group-owned** passes.
3. Players create game passes on their own account
   (Create → Game → Passes, or on their profile), set them **On Sale** with a
   price, then paste the **Pass ID** into **My Stand** in-game.

That's it — donations now flow player-to-player automatically.

---

## What's included

| System | File | What it does |
| --- | --- | --- |
| Config | `src/shared/Config.luau` | All tunables + the code-drawn theme + event list |
| Networking | `src/shared/Net.luau` | Creates/looks up the RemoteEvents & Functions |
| Persistence | `src/server/Data.luau` | DataStore profiles, offline seller crediting, autosave |
| Donations | `src/server/Donations.luau` | Pass linking, ownership checks, purchase crediting |
| Leaderboards | `src/server/Leaderboards.luau` | Global Top Earners / Top Donors (OrderedDataStore) |
| World | `src/server/Stands.luau` | Procedural plaza, per-player stands, physical boards |
| Events | `src/server/Events.luau` | Rotating live events + mini-competitions |
| Dashboard | `src/server/Admin.luau` | Owner-only performance view |
| Bootstrap | `src/server/init.server.luau` | Wires everything together |
| UI | `src/client/init.client.luau` | The entire interface, built in code |

### Live events

Rotating, server-wide events (configure in `Config.EventList`):

- **💸 Donation Rush** — a timed window; whoever donates the most wins the crown.
- **🌟 Donor Spotlight** — highlights a top earner's stand.
- **🍀 Lucky Stand** — picks a random stand to shine.
- **🎉 Generosity Hour** — a longer giving competition.

Winners are announced to everyone via the on-screen banner + countdown.

---

## Rebuilding the place file

The place file is generated from the source tree, so edit the `.luau` files and
regenerate:

```bash
python3 build/build_rbxlx.py
```

This rewrites `DonationCentral.rbxlx`. No Roblox tooling or network needed.

### Optional: a true binary `.rbxl` with Rojo

If you use [Rojo](https://rojo.space), a `default.project.json` is included so
you can live-sync into Studio or build a binary place:

```bash
rojo build -o DonationCentral.rbxl
```

---

## License

See `LICENSE` (GPL-3.0).

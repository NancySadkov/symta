# Symta has moved to https://symta.aermia.com

This repository is no longer maintained on GitHub.  Development
continues at **https://symta.aermia.com**, on infrastructure we
directly administer.  This file is the forwarding pointer; it
will remain in place on the legacy GitHub copy as a permanent
redirect.

## Why we moved

Three reasons, none of them adversarial:

1. **Account policy.**  GitHub now requires two-factor
   authentication for accounts that contribute code.  The policy
   is reasonable in aggregate, but for a single-maintainer
   project we'd rather keep authentication choices in our own
   hands than have them set for us.

2. **Platform scope.**  Over the years GitHub has grown into a
   discussion-and-social layer on top of code hosting.  That's
   helpful for many projects.  For one whose core artefact is a
   programming language and its runtime, the surrounding
   features are unused weight and an evolving distraction.

3. **Infrastructure needs.**  Symta needs more than a git
   repository: an online REPL, downloadable packages, language
   documentation, and a stable URL we can build on.
   Consolidating those onto one server we operate is structurally
   cleaner than coordinating them across a third-party platform
   we don't.

## Conflicting goals

These differences came into focus against three things we want
for the long term:

- A version control workflow that stays the same shape it
  had in the 1990s -- a remote, a push, a clone.  No surrounding
  product layer to track.
- A project home where the language, its REPL, its docs,
  and its packages all live behind one URL we own.
- Minimum vendor coupling.  If any single piece of our
  hosting stack disappears tomorrow, the recovery should be
  `tar`, `rsync`, and a DNS update -- not a multi-week
  re-platforming.

The mismatch isn't anyone's fault.  GitHub is solving for a
different population of users than Symta is part of, and that's
fine.  We just landed on the other side of the line.

## What stays the same

- The **code is unchanged**.  Same source tree, same commit
  history, same license (Apache 2.0 / MIT dual).
- A **public read-only mirror** at
  https://codeberg.org/nancysadkov/symta remains available for
  discoverability, so anyone searching for the language can still
  find it without visiting our self-hosted server first.
- This file remains in the legacy GitHub repository for at least
  twelve months, so existing links continue to land somewhere
  useful.

## Gratitude

GitHub hosted Symta through its early years -- for free, with
excellent reliability, and with the kind of polish that lets a
solo developer focus on the work rather than the infrastructure.
That mattered, and we are sincerely grateful for the years of
free hosting and the discoverability the platform provided.

We hold no grievance against GitHub, against Microsoft, or
against any of GitHub's users.  We wish the platform and its
community continued success.  This move is about fit, not fault.

## Where to find us now

- **Project home:** https://symta.aermia.com
- **Source mirror (read-only):** https://codeberg.org/nancysadkov/symta
- **Contact / issues:** see the project home

To repoint an existing clone:

```sh
# Visit https://symta.aermia.com for the current git URL,
# then:
git remote set-url origin <new-url>
git fetch origin
```

Thank you for stopping by.

-- Nancy Sadkov

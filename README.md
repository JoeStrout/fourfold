# Fourfold

_A dynamic web server powered by server-side MiniScript_

![Fourfold logo](images/fourfold-logo-128px.png)

**Fourfold** is a development environment for building both static and dynamic websites using [MiniScript](https://miniscript.org).

The name is a reference to the four different file areas which form the core of Fourfold's security model:

- **code** (read-only): contains files like HTML/CSS, JavaScript, Markdown, images, and MiniScript; code can read these but never write here.
- **data** (read/write): code can read and write data and files here, but never *execute* or load pages from this area.
- **dropbox** (write-only): code can write log files and other records here, but can't read from it, making it a safe place to drop sensitive details that should be seen only by the site admin.
- **engine** (config only): code can neither read nor write the special engine-supported files in this area: API secrets and other sensitive details.

On a fully hardened installation, the above restrictions are enforced at the OS level as well as by the engine.  This design means that no matter what code you (as a Fourfold developer) write, or what hackers manage to inject into your forms, it is structurally impossible to make your website run code you didn't intend.

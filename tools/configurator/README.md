# Configurator

Static web page, no build step and no server needed: open `index.html` in Chrome or Edge
(also straight from the file system).

* **Open SD card folder** – reads and writes `config.ultrabip` / `config_ext.ultrabip` directly on
  the SD card drive (File System Access API).
* **Connect via Bluetooth** – reads and writes both files through the device's file service
  (`docs/ble-file-service.md`, Web Bluetooth).
* **New config** – no device at hand: the defaults with three example profiles and every view of the
  configurator; saving downloads `config.ultrabip` and `config_ext.ultrabip` (any browser).

Only changed values are rewritten; comments, unknown keys and the order of the lines stay as they
are, so BipLink and the stock configurator keep working with `config.ultrabip`.
`examples/config.ultrabip` is a neutral stock-format file for trying the page out.

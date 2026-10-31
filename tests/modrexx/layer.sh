# layer.sh -- sourced by the tests of the mod_rexx layer: load it into
# Apache (layer_on) and take it out again (layer_off). The layer must be
# installed (make install-modrexx). Apache reads mods-enabled/*.load in
# the order of their names: orx_rexx.load comes after orx.load, so the
# layer is loaded after mod_orx; with ORX_REXX_FIRST=1 it is loaded just
# before it (as orx-rexx.load: "-" sorts before "."), which must work the
# same, since its hooks name the core as their successor (but in Trans and
# Type, where they name nothing, and the core's run early). Just before:
# against the other modules, whose hooks it does not name, the order of
# LoadModule decides, as it did for mod_rexx (in type_checker, loading it
# before mod_mime would let it see every request untyped). The caller
# restarts Apache.
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

layer_on() {
    echo "LoadModule orx_rexx_module $(apxs -q LIBEXECDIR)/mod_orx_rexx.so" \
         > /etc/apache2/mods-available/orx_rexx.load
    rm -f /etc/apache2/mods-enabled/orx_rexx.load /etc/apache2/mods-enabled/orx-rexx.load
    if [ "$ORX_REXX_FIRST" = 1 ]; then
        ln -s ../mods-available/orx_rexx.load /etc/apache2/mods-enabled/orx-rexx.load
        echo "   (the mod_rexx layer is loaded before mod_orx)"
    else
        a2enmod orx_rexx >/dev/null
    fi
}

layer_off() {
    rm -f /etc/apache2/mods-enabled/orx_rexx.load /etc/apache2/mods-enabled/orx-rexx.load
}

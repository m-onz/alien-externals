#!/usr/bin/make -f
# Makefile for the 'alien-externals' signal (~) objects for Pure Data.
# Uses pd-lib-builder: https://github.com/pure-data/pd-lib-builder
#
# Quick start:
#   make                          # build every external for this platform
#   make install                  # install to ~/Documents/Pd/externals
#   make clean                    # remove build products
#
# See README.md for per-platform notes (macOS universal binaries, Windows, etc).

lib.name = alien-externals

# Default install destination for a bare `make install`. Override on the
# command line with `objectsdir=...`. Do NOT use sudo (it sets HOME=/var/root).
objectsdir ?= $(HOME)/Documents/Pd/externals

# One binary per source; the class name == source basename. Every object here
# is a signal (~) external and provides a matching <name>_tilde_setup loader.
class.sources = \
    alien_addchord~.c \
    alien_arp~.c \
    alien_banded~.c \
    alien_bass~.c \
    alien_bitwise~.c \
    alien_bow~.c \
    alien_ca~.c \
    alien_chant~.c \
    alien_chaos~.c \
    alien_cheby~.c \
    alien_chord~.c \
    alien_cloud~.c \
    alien_cml~.c \
    alien_concat~.c \
    alien_cosmic~.c \
    alien_dsf~.c \
    alien_dual~.c \
    alien_feedback~.c \
    alien_feednet~.c \
    alien_fmchord~.c \
    alien_forage~.c \
    alien_formant~.c \
    alien_gendy~.c \
    alien_glisson~.c \
    alien_gnarl~.c \
    alien_hybrid~.c \
    alien_impact~.c \
    alien_iter~.c \
    alien_kuramoto~.c \
    alien_lead~.c \
    alien_liquid~.c \
    alien_liquid1~.c \
    alien_liquid2~.c \
    alien_liquid3~.c \
    alien_logic~.c \
    alien_mass~.c \
    alien_massx~.c \
    alien_membrane~.c \
    alien_membranex~.c \
    alien_modal~.c \
    alien_morph~.c \
    alien_mpe~.c \
    alien_neuro~.c \
    alien_pd~.c \
    alien_pluck~.c \
    alien_pulsar~.c \
    alien_pvoc~.c \
    alien_reese~.c \
    alien_resample~.c \
    alien_risset~.c \
    alien_rot~.c \
    alien_scan~.c \
    alien_shift~.c \
    alien_sms~.c \
    alien_spectra~.c \
    alien_squelch~.c \
    alien_stack~.c \
    alien_stackx~.c \
    alien_talk~.c \
    alien_terrain~.c \
    alien_under~.c \
    alien_unison~.c \
    alien_vector~.c \
    alien_voicing~.c \
    alien_vps~.c \
    alien_walsh~.c \
    alien_warp~.c \
    alien_wgnet~.c \
    alien_wob~.c \
    alien_wtchord~.c \
    alien_xfreesnap_ep02000_05~.c \
    alien_xfreesnap_ep02000_06~.c \
    alien_xfreesnap_ep02000_07~.c \
    alien_xfreesnap_ep02000_08~.c \
    alien_xfreesnap_ep03000_01~.c \
    alien_xfreesnap_ep03000_09~.c \
    alien_xfreesnap_ep04000_04~.c \
    alien_xfreesnap_ep04000_05~.c \
    alien_xfreesnap_ep04000_09~.c \
    alien_xfreesnap_ep04000_10~.c \
    alien_xfreesnap_ep05000_03~.c \
    alien_xfreesnap_ep05000_07~.c \
    alien_xfreesnap_ep05000_08~.c \
    alien_xfreesnap_ep05000_09~.c \
    alien_xfreesnap_ep06000_07~.c \
    alien_xfreesnap_ep07000_01~.c \
    alien_xfreesnap_ep07000_02~.c \
    alien_xfreesnap_ep07000_05~.c \
    alien_xfreesnap_ep07000_08~.c \
    alien_xfreesnap_ep07000_09~.c \
    alien_xfreesnap_ep07000_10~.c \
    alien_xfreesnap_ep08000_01~.c \
    alien_xfreesnap_ep08000_02~.c \
    alien_xfreesnap_ep08000_03~.c \
    alien_xfreesnap_ep08000_10~.c \
    alien_xfreesnap_ep09000_07~.c \
    alien_xfreesnap_ep10000_08~.c \
    alien_xfreesnap_ep10000_09~.c \
    alien_xfreesnap_ep10000_10~.c \
    alien_xfx01~.c \
    alien_xfx02~.c \
    alien_xfx03~.c \
    alien_xfx04~.c \
    alien_xfx05~.c \
    alien_xfx06~.c \
    alien_xfx07~.c \
    alien_xfx08~.c \
    alien_xfx09~.c \
    alien_xfx10~.c \
    alien_xfx11~.c \
    alien_xfx12~.c \
    alien_xfx13~.c \
    alien_xfx14~.c \
    alien_xfx15~.c \
    alien_xfx16~.c \
    alien_xfx17~.c \
    alien_xfx18~.c \
    alien_xfx19~.c \
    alien_xfx20~.c \
    alien_xsnap_ep00012_01~.c \
    alien_xsnap_ep00018_01~.c \
    alien_xsnap_ep01000_01~.c \
    alien_xsnap_ep02000_10~.c \
    alien_xsnap_ep03000_02~.c \
    alien_xsnap_ep03000_08~.c \
    alien_xsnap_ep05000_03~.c \
    alien_xsnap_ep05000_05~.c \
    alien_xsnap_ep05000_09~.c \
    alien_xsnap_ep06000_06~.c \
    alien_xsnap_ep06000_07~.c \
    alien_xsnap_ep07000_04~.c \
    alien_xsnap_ep08000_07~.c

# Extra files copied into the libdir on `make install` (help patches, etc).
datafiles =

include Makefile.pdlibbuilder

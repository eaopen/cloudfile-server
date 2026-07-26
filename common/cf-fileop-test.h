/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * A deliberately dumb write lifecycle provider, for gating the seam itself.
 *
 * Why this exists
 *
 * The seam's exit criterion is "with no lock implemented yet, a fake provider
 * can already refuse at every write entry point". Without something to
 * register, the only evidence the seam is reached at runtime would be the
 * absence of symptoms -- and the directory ACL already showed what that is
 * worth: 62 C checks and 87 Python checks all passed while the rules being
 * stored could never match, and the defect only appeared when the stack came
 * up and another user's token hit each entry point.
 *
 * So this is not a stub standing in for missing work. It is the instrument
 * that proves the wiring, and it stays useful after the lock lands: a lock
 * refuses for reasons that depend on lock state, which makes it a poor probe
 * for "was this call site reached at all".
 *
 * Why it is gated at runtime rather than compiled out
 *
 * A build flag would mean the end-to-end gate exercises an image that is not
 * the one shipped, and this project has already paid for that mistake once --
 * a Django settings test whose fixture was hand-written rather than generated
 * passed while the real generated file raised NameError and silently discarded
 * every CloudFile setting. Test what ships.
 *
 * The cost of that choice is a switch that must never be on in production.
 * It is off unless [cloudfile] fileop_test_provider_enabled is explicitly
 * true, and registering it logs a warning that says so.
 *
 * Configuration, all in the [cloudfile] section of seafile.conf:
 *
 *   fileop_test_provider_enabled   off unless true
 *   fileop_test_refuse_token       refuse any operation whose subject or
 *                                  source path contains this path component;
 *                                  empty means never refuse
 *   fileop_test_journal            append one line per event here; empty
 *                                  means do not journal
 *
 * Contract: cloudfile-docker/docs/fileop-lifecycle.md
 */

#ifndef CF_FILEOP_TEST_H
#define CF_FILEOP_TEST_H

/* Registers the provider iff its switch is on. Called from cf_ext_init(). */
void cf_fileop_test_init (void);

#endif /* CF_FILEOP_TEST_H */

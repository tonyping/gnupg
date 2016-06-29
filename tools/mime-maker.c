/* mime-maker.c - Create MIME structures
 * Copyright (C) 2016 g10 Code GmbH
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "zb32.h"
#include "mime-maker.h"


/* An object to store an header.  Also used for a list of headers.  */
struct header_s
{
  struct header_s *next;
  char *value;   /* Malloced value.  */
  char name[1]; /* Name.  */
};
typedef struct header_s *header_t;


/* An object to store a MIME part.  A part is the header plus the
 * content (body). */
struct part_s
{
  struct part_s *next;  /* Next part in the current container.  */
  struct part_s *child; /* Child container.  */
  char *mediatype;      /* Mediatype of the container (malloced). */
  char *boundary;       /* Malloced boundary string.  */
  header_t headers;     /* List of headers.  */
  header_t *headers_tail;/* Address of last header in chain.  */
  size_t bodylen;       /* Length of BODY.   */
  char *body;           /* Malloced buffer with the body.  This is the
                         * non-encoded value.  */
};
typedef struct part_s *part_t;



/* Definition of the mime parser object.  */
struct mime_maker_context_s
{
  void *cookie;                /* Cookie passed to all callbacks.  */

  unsigned int verbose:1;      /* Enable verbose mode.  */
  unsigned int debug:1;        /* Enable debug mode.  */

  part_t mail;                 /* The MIME tree.  */
  part_t current_part;

  int boundary_counter;  /* Used to create easy to read boundaries.  */
  char *boundary_suffix; /* Random string used in the boundaries.  */

  struct b64state *b64state;     /* NULL or malloced Base64 decoder state.  */

  /* Helper to convey the output stream to recursive functions. */
  estream_t outfp;
};


/* Create a new mime make object.  COOKIE is a values woich will be
 * used as first argument for all callbacks registered with this
 * object.  */
gpg_error_t
mime_maker_new (mime_maker_t *r_maker, void *cookie)
{
  mime_maker_t ctx;

  *r_maker = NULL;

  ctx = xtrycalloc (1, sizeof *ctx);
  if (!ctx)
    return gpg_error_from_syserror ();
  ctx->cookie = cookie;

  *r_maker = ctx;
  return 0;
}


static void
release_parts (part_t part)
{
  while (part)
    {
      part_t partnext = part->next;
      while (part->headers)
        {
          header_t hdrnext = part->headers->next;
          xfree (part->headers);
          part->headers = hdrnext;
        }
      release_parts (part->child);
      xfree (part->mediatype);
      xfree (part->boundary);
      xfree (part->body);
      xfree (part);
      part = partnext;
    }
}


/* Release a mime maker object.  */
void
mime_maker_release (mime_maker_t ctx)
{
  if (!ctx)
    return;

  release_parts (ctx->mail);
  xfree (ctx->boundary_suffix);
  xfree (ctx);
}


/* Set verbose and debug mode.  */
void
mime_maker_set_verbose (mime_maker_t ctx, int level)
{
  if (!level)
    {
      ctx->verbose = 0;
      ctx->debug = 0;
    }
  else
    {
      ctx->verbose = 1;
      if (level > 10)
        ctx->debug = 1;
    }
}


static void
dump_parts (part_t part, int level)
{
  header_t hdr;

  for (; part; part = part->next)
    {
      log_debug ("%*s[part]\n", level*2, "");
      for (hdr = part->headers; hdr; hdr = hdr->next)
        {
          log_debug ("%*s%s: %s\n", level*2, "", hdr->name, hdr->value);
        }
      log_debug ("%*s[body %zu bytes]\n", level*2, "", part->bodylen);
      if (part->child)
        {
          log_debug ("%*s[container]\n", level*2, "");
          dump_parts (part->child, level+1);
        }
    }
}


/* Dump the mime tree for debugging.  */
void
mime_maker_dump_tree (mime_maker_t ctx)
{
  dump_parts (ctx->mail, 0);
}


/* Find the parent node for NEEDLE starting at ROOT.  */
static part_t
find_parent (part_t root, part_t needle)
{
  part_t node, n;

  for (node = root->child; node; node = node->next)
    {
      if (node == needle)
        return root;
      if ((n = find_parent (node, needle)))
        return n;
    }
  return NULL;
}


/* Create a boundary string.  Outr codes is aware of the general
 * structure of that string (gebins with "=-=") so that
 * it can protect against accidently used boundaries within the
 * content.   */
static char *
generate_boundary (mime_maker_t ctx)
{
  if (!ctx->boundary_suffix)
    {
      char buffer[12];

      gcry_create_nonce (buffer, sizeof buffer);
      ctx->boundary_suffix = zb32_encode (buffer, 8 * sizeof buffer);
      if (!ctx->boundary_suffix)
        return NULL;
    }

  ctx->boundary_counter++;
  return es_bsprintf ("=-=%02d-%s=-=",
                      ctx->boundary_counter, ctx->boundary_suffix);
}


/* Ensure that the context has a MAIL and CURRENT_PART object and
 * return the parent object if available  */
static gpg_error_t
ensure_part (mime_maker_t ctx, part_t *r_parent)
{
  if (!ctx->mail)
    {
      ctx->mail = xtrycalloc (1, sizeof *ctx->mail);
      if (!ctx->mail)
        return gpg_error_from_syserror ();
      log_assert (!ctx->current_part);
      ctx->current_part = ctx->mail;
      ctx->current_part->headers_tail = &ctx->current_part->headers;
    }
  log_assert (ctx->current_part);
  if (r_parent)
    *r_parent = find_parent (ctx->mail, ctx->current_part);

  return 0;
}


/* Transform a header name into a standard capitalized format.
 * "Content-Type".  Conversion stops at the colon. */
static void
capitalize_header_name (char *name)
{
  unsigned char *p = name;
  int first = 1;

  /* Special cases first.  */
  if (!ascii_strcasecmp (name, "MIME-Version"))
    {
      strcpy (name, "MIME-Version");
      return;
    }

  /* Regular cases.  */
  for (; *p && *p != ':'; p++)
    {
      if (*p == '-')
        first = 1;
      else if (first)
        {
          if (*p >= 'a' && *p <= 'z')
            *p = *p - 'a' + 'A';
          first = 0;
        }
      else if (*p >= 'A' && *p <= 'Z')
        *p = *p - 'A' + 'a';
    }
}


/* Check whether a header with NAME has already been set into PART.
 * NAME must be in canonical capitalized format.  Return true or
 * false. */
static int
have_header (part_t part, const char *name)
{
  header_t hdr;

  for (hdr = part->headers; hdr; hdr = hdr->next)
    if (!strcmp (hdr->name, name))
      return 1;
  return 0;
}


/* Helper to add a header to a part.  */
static gpg_error_t
add_header (part_t part, const char *name, const char *value)
{
  gpg_error_t err;
  header_t hdr;

  hdr = xtrymalloc (sizeof *hdr + strlen (name));
  if (!hdr)
    return gpg_error_from_syserror ();
  hdr->next = NULL;
  strcpy (hdr->name, name);
  capitalize_header_name (hdr->name);
  hdr->value = xtrystrdup (value);
  if (!hdr->value)
    {
      err = gpg_error_from_syserror ();
      xfree (hdr);
      return err;
    }
  *part->headers_tail = hdr;
  part->headers_tail = &hdr->next;

  return 0;
}


/* Add a header with NAME and VALUE to the current mail.  A LF in the
 * VALUE will be handled automagically.  If no container has been
 * added, the header will be used for the regular mail headers and not
 * for a MIME part.  If the current part is in a container and a body
 * has been added, we append a new part to the current container.
 * Thus for a non-MIME mail the caller needs to call this function
 * followed by a call to add a body.  When adding a Content-Type the
 * boundary parameter must not be included.
 */
gpg_error_t
mime_maker_add_header (mime_maker_t ctx, const char *name, const char *value)
{
  gpg_error_t err;
  part_t part, parent;

  err = ensure_part (ctx, &parent);
  if (err)
    return err;
  part = ctx->current_part;

  if (part->body && !parent)
    {
      /* We already have a body but no parent.  Adding another part is
       * thus not possible.  */
      return gpg_error (GPG_ERR_CONFLICT);
    }
  if (part->body)
    {
      /* We already have a body and there is a parent.  We now append
       * a new part to the current container.  */
      part = xtrycalloc (1, sizeof *part);
      if (!part)
        return gpg_error_from_syserror ();
      part->headers_tail = &part->headers;
      log_assert (!ctx->current_part->next);
      ctx->current_part->next = part;
      ctx->current_part = part;
    }

  /* If no NAME and no VALUE has been given we do not add a header.
   * This can be used to create a new part without any header.  */
  if (!name && !value)
    return 0;

  /* If we add Content-Type, make sure that we have a MIME-version
   * header first; this simply looks better.  */
  if (!ascii_strcasecmp (name, "Content-Type")
      && !have_header (ctx->mail, "MIME-Version"))
    {
      err = add_header (ctx->mail, "MIME-Version", "1.0");
      if (err)
        return err;
    }
  return add_header (part, name, value);
}


/* Helper for mime_maker_add_{body,stream}.  */
static gpg_error_t
add_body (mime_maker_t ctx, const void *data, size_t datalen)
{
  gpg_error_t err;
  part_t part, parent;

  err = ensure_part (ctx, &parent);
  if (err)
    return err;
  part = ctx->current_part;
  if (part->body)
    return gpg_error (GPG_ERR_CONFLICT);

  part->body = xtrymalloc (datalen? datalen : 1);
  if (!part->body)
    return gpg_error_from_syserror ();
  part->bodylen = datalen;
  if (data)
    memcpy (part->body, data, datalen);

  return 0;
}


/* Add STRING as body to the mail or the current MIME container.  A
 * second call to this function is not allowed.
 *
 * FIXME: We may want to have an append_body to add more data to a body.
 */
gpg_error_t
mime_maker_add_body (mime_maker_t ctx, const char *string)
{
  return add_body (ctx, string, strlen (string));
}


/* This is the same as mime_maker_add_body but takes a stream as
 * argument.  As of now the stream is copied to the MIME object but
 * eventually we may delay that and read the stream only at the time
 * it is needed.  Note that the address of the stream object must be
 * passed and that the ownership of the stream is transferred to this
 * MIME object.  To indicate the latter the function will store NULL
 * at the ADDR_STREAM so that a caller can't use that object anymore
 * except for es_fclose which accepts a NULL pointer.  */
gpg_error_t
mime_maker_add_stream (mime_maker_t ctx, estream_t *stream_addr)
{
  void *data;
  size_t datalen;

  es_rewind (*stream_addr);
  if (es_fclose_snatch (*stream_addr, &data, &datalen))
    return gpg_error_from_syserror ();
  *stream_addr = NULL;
  return add_body (ctx, data, datalen);
}


/* Add a new MIME container.  The caller needs to provide the media
 * and media-subtype in MEDIATYPE.  If MEDIATYPE is NULL
 * "multipart/mixed" is assumed.  This function will then add a
 * Content-Type header with that media type and an approriate boundary
 * string to the parent part.  */
gpg_error_t
mime_maker_add_container (mime_maker_t ctx, const char *mediatype)
{
  gpg_error_t err;
  part_t part;

  if (!mediatype)
    mediatype = "multipart/mixed";

  err = ensure_part (ctx, NULL);
  if (err)
    return err;
  part = ctx->current_part;
  if (part->body)
    return gpg_error (GPG_ERR_CONFLICT); /* There is already a body. */
  if (part->child || part->mediatype || part->boundary)
    return gpg_error (GPG_ERR_CONFLICT); /* There is already a container. */

  /* If a content type has not yet been set, do it now.  The boundary
   * will be added while writing the headers.  */
  if (!have_header (ctx->mail, "Content-Type"))
    {
      err = add_header (ctx->mail, "Content-Type", mediatype);
      if (err)
        return err;
    }

  /* Create a child node.  */
  part->child = xtrycalloc (1, sizeof *part->child);
  if (!part->child)
    return gpg_error_from_syserror ();
  part->child->headers_tail = &part->child->headers;

  part->mediatype = xtrystrdup (mediatype);
  if (!part->mediatype)
    {
      err = gpg_error_from_syserror ();
      xfree (part->child);
      part->child = NULL;
      return err;
    }

  part->boundary = generate_boundary (ctx);
  if (!part->boundary)
    {
      err = gpg_error_from_syserror ();
      xfree (part->child);
      part->child = NULL;
      xfree (part->mediatype);
      part->mediatype = NULL;
      return err;
    }

  part = part->child;
  ctx->current_part = part;

  return 0;
}


/* Write the Content-Type header with the boundary value.  */
static gpg_error_t
write_ct_with_boundary (mime_maker_t ctx,
                        const char *value, const char *boundary)
{
  const char *s;

  if (!*value)
    return gpg_error (GPG_ERR_INV_VALUE);  /* Empty string.  */

  for (s=value + strlen (value) - 1;
       (s >= value
        && (*s == ' ' || *s == '\t' || *s == '\n'));
       s--)
    ;
  if (!(s >= value))
    return gpg_error (GPG_ERR_INV_VALUE);  /* Only spaces.  */

  /* Fixme: We should use a dedicated header write functions which
   * properly wraps the header.  */
  es_fprintf (ctx->outfp, "Content-Type: %s%s\n\tboundary=\"%s\"\n",
              value,
              (*s == ';')? "":";",
              boundary);
  return 0;
}


/* Recursive worker for mime_maker_make.  */
static gpg_error_t
write_tree (mime_maker_t ctx, part_t parent, part_t part)
{
  gpg_error_t err;
  header_t hdr;

  for (; part; part = part->next)
    {
      for (hdr = part->headers; hdr; hdr = hdr->next)
        {
          if (part->child && !strcmp (hdr->name, "Content-Type"))
            write_ct_with_boundary (ctx, hdr->value, part->boundary);
          else
            es_fprintf (ctx->outfp, "%s: %s\n", hdr->name, hdr->value);
        }
      es_fputc ('\n', ctx->outfp);
      if (part->body)
        {
          if (es_write (ctx->outfp, part->body, part->bodylen, NULL))
            return gpg_error_from_syserror ();
        }
      if (part->child)
        {
          log_assert (part->boundary);
          if (es_fprintf (ctx->outfp, "\n--%s\n", part->boundary) < 0)
            return gpg_error_from_syserror ();
          err = write_tree (ctx, part, part->child);
          if (err)
            return err;
          if (es_fprintf (ctx->outfp, "\n--%s--\n", part->boundary) < 0)
            return gpg_error_from_syserror ();
        }

      if (part->next)
        {
          log_assert (parent && parent->boundary);
          if (es_fprintf (ctx->outfp, "\n--%s\n", parent->boundary) < 0)
            return gpg_error_from_syserror ();
        }
    }
  return 0;
}


/* Add headers we always require.  */
static gpg_error_t
add_missing_headers (mime_maker_t ctx)
{
  gpg_error_t err;

  if (!ctx->mail)
    return gpg_error (GPG_ERR_NO_DATA);
  if (!have_header (ctx->mail, "MIME-Version"))
    {
      /* Even if a Content-Type has never been set, we want to
       * announce that we do MIME.  */
      err = add_header (ctx->mail, "MIME-Version", "1.0");
      if (err)
        goto leave;
    }

  if (!have_header (ctx->mail, "Date"))
    {
      char *p = rfctimestamp (make_timestamp ());
      if (!p)
        err = gpg_error_from_syserror ();
      else
        err = add_header (ctx->mail, "Date", p);
      xfree (p);
      if (err)
        goto leave;
    }


 leave:
  return err;
}


/* Create message from the tree MIME and write it to FP.  Noet that
 * the output uses only a LF and a later called sendmail(1) is
 * expected to convert them to network line endings.  */
gpg_error_t
mime_maker_make (mime_maker_t ctx, estream_t fp)
{
  gpg_error_t err;

  err = add_missing_headers (ctx);
  if (err)
    return err;

  ctx->outfp = fp;
  err = write_tree (ctx, NULL, ctx->mail);

  ctx->outfp = NULL;
  return err;
}

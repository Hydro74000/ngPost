# Post info files

After each post, ngPost can write a small text file describing what was just
posted. Some Usenet indexes ask for such a file to reference a post.

**ngPost knows no index format.** You give it a model, it fills in the blanks.
That is why these files live here and not in the source code: if an index
changes its format, only its model changes.

## Use one

1. Copy the model you want (or its content) **anywhere you can read it**. It
   does not have to be in this folder, nor in any particular one.
2. Add two lines to your `ngPost.conf`:

```ini
POST_INFO_TEMPLATE = my_sheet.txt
POST_INFO_OUTPUT = __nzbDir__/__nzbName__.info.txt
```

A bare file name is understood from the folder of your `ngPost.conf`. **If the
model lives anywhere else, write its full path**, ngPost will not go looking
for it:

```ini
POST_INFO_TEMPLATE = /home/me/models/my_sheet.txt
POST_INFO_TEMPLATE = C:\Users\me\models\my_sheet.txt
```

On the command line, `--post_info_template` reads a relative path from the
folder you are standing in instead.

3. Post something, and look next to your `.nzb`.

## The models here

| File | What it is |
|---|---|
| `post_info_default.txt` | generic, one line per available variable, no password |
| `post_info_baselien.txt` | the exact format expected by the Baselien index |
| `post_info_json.txt` | a JSON sheet, for an index with an HTTP API |

A model is free text, so it can be a JSON or an XML document too. Declare the
format on a line of its own — `#!json` or `#!xml` — or simply name the model
`something.json` or `something.xml`, which says the same thing. The declaration
wins when both are present. ngPost then escapes every value it inserts, so a
title holding a quote or an ampersand can no longer break the file. It escapes the values only: the braces, the tags and the field
names are yours. Without a declaration nothing is escaped, which is what lets
a text sheet use any separator you like.

## Full documentation

The complete beginner guide, with every variable and a worked example, is on
the wiki: **Post info files**.

\echo Use "ALTER EXTENSION laplace UPDATE TO '1.17'" to load this file. \quit
COMMENT ON FUNCTION laplace_text_id(text) IS
  'Canonical Native UAX #29 codepoint/grapheme/word/phrase composition, identical to Engine text ingestion. Empty content returns NULL; source bytes are not normalized.';

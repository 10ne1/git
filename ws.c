/*
 * Whitespace rules
 *
 * Copyright (c) 2007 Junio C Hamano
 */

#define DISABLE_SIGN_COMPARE_WARNINGS

#include "git-compat-util.h"
#include "attr.h"
#include "strbuf.h"
#include "ws.h"

unsigned whitespace_rule_cfg = WS_DEFAULT_RULE;

static struct whitespace_rule {
	const char *rule_name;
	unsigned rule_bits;
	unsigned loosens_error:1,
		exclude_default:1;
} whitespace_rule_names[] = {
	{ "trailing-space", WS_TRAILING_SPACE, 0 },
	{ "space-before-tab", WS_SPACE_BEFORE_TAB, 0 },
	{ "indent-with-non-tab", WS_INDENT_WITH_NON_TAB, 0 },
	{ "cr-at-eol", WS_CR_AT_EOL, 1 },
	{ "blank-at-eol", WS_BLANK_AT_EOL, 0 },
	{ "blank-at-eof", WS_BLANK_AT_EOF, 0 },
	{ "tab-in-indent", WS_TAB_IN_INDENT, 0, 1 },
	{ "tab-between-non-ws", WS_TAB_BETWEEN_NON_WS, 0 },
	{ "incomplete-line", WS_INCOMPLETE_LINE, 0, 0 },
};

unsigned parse_whitespace_rule(const char *string)
{
	unsigned rule = WS_DEFAULT_RULE;

	while (string) {
		int i;
		size_t len;
		const char *ep;
		const char *arg;
		int negated = 0;

		string = string + strspn(string, ", \t\n\r");
		ep = strchrnul(string, ',');
		len = ep - string;

		if (*string == '-') {
			negated = 1;
			string++;
			len--;
		}
		if (!len)
			break;
		for (i = 0; i < ARRAY_SIZE(whitespace_rule_names); i++) {
			if (strncmp(whitespace_rule_names[i].rule_name,
				    string, len))
				continue;
			if (negated)
				rule &= ~whitespace_rule_names[i].rule_bits;
			else
				rule |= whitespace_rule_names[i].rule_bits;
			break;
		}
		if (skip_prefix(string, "tabwidth=", &arg)) {
			unsigned tabwidth = atoi(arg);
			if (0 < tabwidth && tabwidth < 0100) {
				rule &= ~WS_TAB_WIDTH_MASK;
				rule |= tabwidth;
			}
			else
				warning("tabwidth %.*s out of range",
					(int)(ep - arg), arg);
		}
		string = ep;
	}

	if (rule & WS_TAB_IN_INDENT && rule & WS_INDENT_WITH_NON_TAB)
		die("cannot enforce both tab-in-indent and indent-with-non-tab");
	return rule;
}

unsigned whitespace_rule(struct index_state *istate, const char *pathname)
{
	static struct attr_check *attr_whitespace_rule;
	const char *value;

	if (!attr_whitespace_rule)
		attr_whitespace_rule = attr_check_initl("whitespace", NULL);

	git_check_attr(istate, pathname, attr_whitespace_rule);
	value = attr_whitespace_rule->items[0].value;
	if (ATTR_TRUE(value)) {
		/* true (whitespace) */
		unsigned all_rule = ws_tab_width(whitespace_rule_cfg);
		int i;
		for (i = 0; i < ARRAY_SIZE(whitespace_rule_names); i++)
			if (!whitespace_rule_names[i].loosens_error &&
			    !whitespace_rule_names[i].exclude_default)
				all_rule |= whitespace_rule_names[i].rule_bits;
		return all_rule;
	} else if (ATTR_FALSE(value)) {
		/* false (-whitespace) */
		return ws_tab_width(whitespace_rule_cfg);
	} else if (ATTR_UNSET(value)) {
		/* reset to default (!whitespace) */
		return whitespace_rule_cfg;
	} else {
		/* string */
		return parse_whitespace_rule(value);
	}
}

/* The returned string should be freed by the caller. */
char *whitespace_error_string(unsigned ws)
{
	struct strbuf err = STRBUF_INIT;
	if ((ws & WS_TRAILING_SPACE) == WS_TRAILING_SPACE)
		strbuf_addstr(&err, "trailing whitespace");
	else {
		if (ws & WS_BLANK_AT_EOL)
			strbuf_addstr(&err, "trailing whitespace");
		if (ws & WS_BLANK_AT_EOF) {
			if (err.len)
				strbuf_addstr(&err, ", ");
			strbuf_addstr(&err, "new blank line at EOF");
		}
	}
	if (ws & WS_SPACE_BEFORE_TAB) {
		if (err.len)
			strbuf_addstr(&err, ", ");
		strbuf_addstr(&err, "space before tab in indent");
	}
	if (ws & WS_INDENT_WITH_NON_TAB) {
		if (err.len)
			strbuf_addstr(&err, ", ");
		strbuf_addstr(&err, "indent with spaces");
	}
	if (ws & WS_TAB_IN_INDENT) {
		if (err.len)
			strbuf_addstr(&err, ", ");
		strbuf_addstr(&err, "tab in indent");
	}
	if (ws & WS_TAB_BETWEEN_NON_WS) {
		if (err.len)
			strbuf_addstr(&err, ", ");
		strbuf_addstr(&err, "tab between non-whitespace characters");
	}
	if (ws & WS_INCOMPLETE_LINE) {
		if (err.len)
			strbuf_addstr(&err, ", ");
		strbuf_addstr(&err, "no newline at the end of file");
	}
	return strbuf_detach(&err, NULL);
}

static void emit_literal(const char *line, int len, FILE *stream)
{
	fwrite(line, len, 1, stream);
}

static void highlight_tabs(const char *line, int len,
			   int written, int trailing_whitespace,
			   unsigned ws_rule, FILE *stream,
			   const char *set, const char *reset, const char *ws)
{
	int tabwidth = ws_tab_width(ws_rule);
	int start = written, col = 0;

	if (!tabwidth)
		BUG("a known tabwidth is required by WS_TAB_BETWEEN_NON_WS");

	/*
	 * Calculate the visual column position (col) up to 'written'.
	 * Tabs expand based on 'tabwidth', so for example the 5th character in a
	 * string might be at the 12th visual column, if the line contains tabs.
	 */
	for (int i = 0; i < written; i++) {
		if (line[i] == '\t')
			col += tabwidth - (col % tabwidth);
		else
			col++;
	}

	/* Iterate through the section of the line that needs potential highlighting. */
	for (int i = written; i < trailing_whitespace; i++) {
		if (line[i] == '\t') {
			if (i > 0 && i < len - 1 &&
			    !isspace(line[i - 1]) && !isspace(line[i + 1]) &&
			    (col % tabwidth) == (tabwidth - 1)) {
				/* Print unwritten content before the highlighted tab. */
				if (start < i)
					emit_literal(line + start, i - start, stream);

				/* Apply highlight for the tab. */
				fputs(reset, stream);
				fputs(ws, stream);
				fputc('\t', stream);
				fputs(reset, stream);
				fputs(set, stream);
				start = i + 1;
			}
			col += tabwidth - (col % tabwidth);
		} else {
			col++; /* non-tab character. */
		}
	}

	/* Print any remaining content in the current segment after the last highlight. */
	if (start < trailing_whitespace)
		emit_literal(line + start, trailing_whitespace - start, stream);
}

static void emit_middle_section(const char *line, int len,
			       int written, int trailing_whitespace,
			       unsigned ws_rule, unsigned result,
			       FILE *stream, const char *set,
			       const char *reset, const char *ws)
{
	fputs(set, stream);

	if (result & WS_TAB_BETWEEN_NON_WS)
		highlight_tabs(line, len, written, trailing_whitespace,
			       ws_rule, stream, set, reset, ws);
	else
		emit_literal(line + written, trailing_whitespace - written, stream);

	fputs(reset, stream);
}

/* If stream is non-NULL, emits the line after checking. */
static unsigned ws_check_emit_1(const char *line, int len, unsigned ws_rule,
				FILE *stream, const char *set,
				const char *reset, const char *ws)
{
	unsigned result = 0;
	int written = 0;
	int trailing_whitespace = -1;
	int trailing_newline = 0;
	int trailing_carriage_return = 0;
	int i;

	/* Logic is simpler if we temporarily ignore the trailing newline. */
	if (len > 0 && line[len - 1] == '\n') {
		trailing_newline = 1;
		len--;
	}
	if ((ws_rule & WS_CR_AT_EOL) &&
	    len > 0 && line[len - 1] == '\r') {
		trailing_carriage_return = 1;
		len--;
	}

	/* Check for trailing whitespace. */
	if (ws_rule & WS_BLANK_AT_EOL) {
		for (i = len - 1; i >= 0; i--) {
			if (isspace(line[i])) {
				trailing_whitespace = i;
				result |= WS_BLANK_AT_EOL;
			}
			else
				break;
		}
	}

	if (trailing_whitespace == -1)
		trailing_whitespace = len;

	if (!trailing_newline && (ws_rule & WS_INCOMPLETE_LINE))
		result |= WS_INCOMPLETE_LINE;

	/* Check indentation */
	for (i = 0; i < trailing_whitespace; i++) {
		if (line[i] == ' ')
			continue;
		if (line[i] != '\t')
			break;
		if ((ws_rule & WS_SPACE_BEFORE_TAB) && written < i) {
			result |= WS_SPACE_BEFORE_TAB;
			if (stream) {
				fputs(ws, stream);
				fwrite(line + written, i - written, 1, stream);
				fputs(reset, stream);
				fwrite(line + i, 1, 1, stream);
			}
		} else if (ws_rule & WS_TAB_IN_INDENT) {
			result |= WS_TAB_IN_INDENT;
			if (stream) {
				fwrite(line + written, i - written, 1, stream);
				fputs(ws, stream);
				fwrite(line + i, 1, 1, stream);
				fputs(reset, stream);
			}
		} else if (stream) {
			fwrite(line + written, i - written + 1, 1, stream);
		}
		written = i + 1;
	}

	/* Check for indent using non-tab. */
	if ((ws_rule & WS_INDENT_WITH_NON_TAB) && i - written >= ws_tab_width(ws_rule)) {
		result |= WS_INDENT_WITH_NON_TAB;
		if (stream) {
			fputs(ws, stream);
			fwrite(line + written, i - written, 1, stream);
			fputs(reset, stream);
		}
		written = i;
	}

	if (ws_rule & WS_TAB_BETWEEN_NON_WS) {
		/*
		 * A tab surrounded by non-whitespace characters is a typo candidate
		 * (a space might have been intended). This checks for a tab that
		 * would be expanded to a single space, which is when it appears at
		 * a column that is one less than a multiple of the tabwidth.
		 */
		int col = 0;
		int tabwidth = ws_tab_width(ws_rule);

		if (!tabwidth)
			BUG("a known tabwidth is required by WS_TAB_BETWEEN_NON_WS");

		for (i = 0; i < len; i++) {
			if (line[i] == '\t') {
				if (i > 0 && i < len - 1 &&
				    !isspace(line[i - 1]) && !isspace(line[i + 1]) &&
				    (col % tabwidth) == (tabwidth - 1))
					result |= WS_TAB_BETWEEN_NON_WS;
				col += tabwidth - (col % tabwidth);
			} else {
				col++;
			}
		}
	}

	if (stream) {
		/*
		 * The middle section of the line starts at "written" and ends at
		 * "trailing_whitespace".
		 */
		if (trailing_whitespace - written > 0)
			emit_middle_section(line, len, written, trailing_whitespace,
					    ws_rule, result,
					    stream, set, reset, ws);

		/* Highlight errors in trailing whitespace. */
		if (trailing_whitespace != len) {
			fputs(ws, stream);
			fwrite(line + trailing_whitespace,
			    len - trailing_whitespace, 1, stream);
			fputs(reset, stream);
		}
		if (trailing_carriage_return)
			fputc('\r', stream);
		if (trailing_newline)
			fputc('\n', stream);
	}
	return result;
}

void ws_check_emit(const char *line, int len, unsigned ws_rule,
		   FILE *stream, const char *set,
		   const char *reset, const char *ws)
{
	(void)ws_check_emit_1(line, len, ws_rule, stream, set, reset, ws);
}

unsigned ws_check(const char *line, int len, unsigned ws_rule)
{
	return ws_check_emit_1(line, len, ws_rule, NULL, NULL, NULL, NULL);
}

int ws_blank_line(const char *line, int len)
{
	/*
	 * We _might_ want to treat CR differently from other
	 * whitespace characters when ws_rule has WS_CR_AT_EOL, but
	 * for now we just use this stupid definition.
	 */
	while (len-- > 0) {
		if (!isspace(*line))
			return 0;
		line++;
	}
	return 1;
}

/* Copy the line onto the end of the strbuf while fixing whitespaces */
void ws_fix_copy(struct strbuf *dst, const char *src, int len, unsigned ws_rule, int *error_count)
{
	/*
	 * len is number of bytes to be copied from src, starting
	 * at src.  Typically src[len-1] is '\n', unless this is
	 * the incomplete last line.
	 */
	int i;
	int add_nl_to_tail = 0;
	int add_cr_to_tail = 0;
	int fixed = 0;
	int last_tab_in_indent = -1;
	int last_space_in_indent = -1;
	int need_fix_leading_space = 0;
	size_t pre_indent_len = dst->len;

	/*
	 * Remembering that we need to add '\n' at the end
	 * is sufficient to fix an incomplete line.
	 */
	if (ws_rule & WS_INCOMPLETE_LINE) {
		if (0 < len && src[len - 1] != '\n') {
			fixed = 1;
			add_nl_to_tail = 1;
		}
	}

	/*
	 * Strip trailing whitespace
	 */
	if (ws_rule & WS_BLANK_AT_EOL) {
		if (0 < len && src[len - 1] == '\n') {
			add_nl_to_tail = 1;
			len--;
			if (0 < len && src[len - 1] == '\r') {
				add_cr_to_tail = !!(ws_rule & WS_CR_AT_EOL);
				len--;
			}
		}
		if (0 < len && isspace(src[len - 1])) {
			while (0 < len && isspace(src[len-1]))
				len--;
			fixed = 1;
		}
	}

	/*
	 * Check leading whitespaces (indent)
	 */
	for (i = 0; i < len; i++) {
		char ch = src[i];
		if (ch == '\t') {
			last_tab_in_indent = i;
			if ((ws_rule & WS_SPACE_BEFORE_TAB) &&
			    0 <= last_space_in_indent)
			    need_fix_leading_space = 1;
		} else if (ch == ' ') {
			last_space_in_indent = i;
			if ((ws_rule & WS_INDENT_WITH_NON_TAB) &&
			    ws_tab_width(ws_rule) <= i - last_tab_in_indent)
				need_fix_leading_space = 1;
		} else
			break;
	}

	if (need_fix_leading_space) {
		/* Process indent ourselves */
		int consecutive_spaces = 0;
		int last = last_tab_in_indent + 1;

		if (ws_rule & WS_INDENT_WITH_NON_TAB) {
			/* have "last" point at one past the indent */
			if (last_tab_in_indent < last_space_in_indent)
				last = last_space_in_indent + 1;
			else
				last = last_tab_in_indent + 1;
		}

		/*
		 * between src[0..last-1], strip the funny spaces,
		 * updating them to tab as needed.
		 */
		for (i = 0; i < last; i++) {
			char ch = src[i];
			if (ch != ' ') {
				consecutive_spaces = 0;
				strbuf_addch(dst, ch);
			} else {
				consecutive_spaces++;
				if (consecutive_spaces == ws_tab_width(ws_rule)) {
					strbuf_addch(dst, '\t');
					consecutive_spaces = 0;
				}
			}
		}
		while (0 < consecutive_spaces--)
			strbuf_addch(dst, ' ');
		len -= last;
		src += last;
		fixed = 1;
	} else if ((ws_rule & WS_TAB_IN_INDENT) && last_tab_in_indent >= 0) {
		/* Expand tabs into spaces */
		int start = dst->len;
		int last = last_tab_in_indent + 1;
		for (i = 0; i < last; i++) {
			if (src[i] == '\t')
				do {
					strbuf_addch(dst, ' ');
				} while ((dst->len - start) % ws_tab_width(ws_rule));
			else
				strbuf_addch(dst, src[i]);
		}
		len -= last;
		src += last;
		fixed = 1;
	}

	if (!(ws_rule & WS_TAB_BETWEEN_NON_WS)) {
		/*
		 * Middle section does not need fixing, so just append src to
		 * dst because it already contains the previous ws fixes.
		 */
		strbuf_add(dst, src, len);
	} else {
		/* Fix middle section HTs which expand to a single display space */
		const char *indent_part = dst->buf + pre_indent_len;
		int indent_len = dst->len - pre_indent_len;
		int tabwidth = ws_tab_width(ws_rule);
		int col = 0;

		if (!tabwidth)
			BUG("a known tabwidth is required by WS_TAB_BETWEEN_NON_WS");

		/*
		 * Compute indentation display column length because it might not
		 * end at a fixed tab stop position.
		 */
		for (i = 0; i < indent_len; i++) {
			if (indent_part[i] == '\t')
				col += tabwidth - (col % tabwidth);
			else
				col++;
		}

		/* Go through all chars in src, fix if necessary, and append to dst */
		for (i = 0; i < len; i++) {
			char prev_ch = i > 0 ? src[i - 1] :
				(indent_len > 0 ? indent_part[indent_len - 1] : '\0');
			char next_ch = i < len - 1 ? src[i + 1] : '\0';
			bool needs_fixing = prev_ch && next_ch &&
				!isspace(prev_ch) && !isspace(next_ch) &&
				(col % tabwidth) == (tabwidth - 1);

			/* non HT chars are added as is */
			if (src[i] != '\t') {
				strbuf_addch(dst, src[i]);
				col++;
				continue;
			}

			/* fix HT or add them as is */
			if (needs_fixing) {
				strbuf_addch(dst, ' ');
				col++;
				fixed = 1;
			} else {
				strbuf_addch(dst, '\t');
				col += tabwidth - (col % tabwidth);
			}
		}
	}

	if (add_cr_to_tail)
		strbuf_addch(dst, '\r');
	if (add_nl_to_tail)
		strbuf_addch(dst, '\n');
	if (fixed && error_count)
		(*error_count)++;
}

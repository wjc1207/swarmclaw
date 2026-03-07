# ArXiv Search

Search for academic papers on ArXiv by keywords using the http_request tool.

## When to use
When the user asks to find academic papers, research articles, preprints, or scientific publications.
Also when the user mentions ArXiv or asks about recent research on a topic.

## How to use
1. Identify the search keywords from the user's query
2. Build the ArXiv API query URL:
   - Base URL: `https://export.arxiv.org/api/query`
   - Add `search_query=` with keywords joined by `+AND+` (URL-encoded spaces as `+`)
   - Use field prefixes: `all:` (any field), `ti:` (title), `au:` (author), `abs:` (abstract), `cat:` (category)
   - Add `&start=0&max_results=5` to limit results
   - Add `&sortBy=submittedDate&sortOrder=descending` for newest first
3. Use http_request tool with method GET and the constructed URL
4. Parse the Atom XML response — each `<entry>` contains:
   - `<title>`: paper title
   - `<summary>`: abstract
   - `<author><name>`: author names
   - `<link>` with `title="pdf"`: PDF link
   - `<published>`: publication date
5. Present results in a clear format: title, authors, date, abstract snippet, and link

## Example
User: "Find recent papers on large language models"
→ http_request url="https://export.arxiv.org/api/query?search_query=all:large+AND+all:language+AND+all:models&start=0&max_results=5&sortBy=submittedDate&sortOrder=descending" method="GET"
→ Parse the XML response and list papers with title, authors, date, and link

User: "Search ArXiv for papers by Yann LeCun on deep learning"
→ http_request url="https://export.arxiv.org/api/query?search_query=au:LeCun+AND+all:deep+learning&start=0&max_results=5&sortBy=submittedDate&sortOrder=descending" method="GET"

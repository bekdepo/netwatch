static const char data_404_html[] =
  "<html><head><title>404 Not Found</title></head>"
  "<body><h1>404 Not Found</h1><p>This is probably not the machine you're looking for...</p></body>"
  "</html>";

static const char data_index_html[] =
  "<html><head><title>NetWatch</title></head>"
  "<body><h1>NetWatch</h1><table cellpadding=0 cellspacing=0 border=0><tr valign=top><td>"
  "<iframe src=\"registers.html\" height=500 width=300></iframe></td><td>"
  "<iframe src=\"backtrace.html\" height=250 width=150></iframe></td></tr></table>"
  "<form action=reboot type=post><input type=submit value=\"Reboot!\"></form>"
  "</body></html>";

const struct fsdata_file file_404_html[] = {{NULL, "/404.html", data_404_html, sizeof(data_404_html)}};
const struct fsdata_file file_index_html[] = {{file_404_html, "/index.html", data_index_html, sizeof(data_index_html)}};

#define FS_ROOT file_index_html

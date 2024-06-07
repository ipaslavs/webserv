# Створення структури директорій
mkdir -p YoupiBanane/nop
mkdir -p YoupiBanane/Yeah

# Створення файлів з простим HTML-контентом
echo "<html><body><h1>youpi.bad_extension</h1></body></html>" > YoupiBanane/youpi.bad_extension
echo "<html><body><h1>youpi.bla</h1></body></html>" > YoupiBanane/youpi.bla
echo "<html><body><h1>youpi.bad_extension in nop</h1></body></html>" > YoupiBanane/nop/youpi.bad_extension
echo "<html><body><h1>other.pouic in nop</h1></body></html>" > YoupiBanane/nop/other.pouic
echo "<html><body><h1>not_happy.bad_extension in Yeah</h1></body></html>" > YoupiBanane/Yeah/not_happy.bad_extension

echo "Script executed successfully."


in development. C++ and JS tool for visualizing long-term rtl_power data.

```
sudo apt-get install libpng-dev
g++ -O3 -Wall -Werror visualize.cpp -o visualize -lpng && (cat 46M.csv.gz | gunzip | time ./visualize)
```

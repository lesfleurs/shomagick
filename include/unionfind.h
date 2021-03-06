#ifndef UNION_FIND_HPP_
#define UNION_FIND_HPP_

#include <vector>

class UnionFind
{
private:
  std::vector<int> p, rank, setSize;
  int numSets;

public:
  UnionFind():p(), rank(), setSize(), numSets(){};
  UnionFind(int n):p(n,0),rank(n,0), setSize(n,1), numSets(n)
  {
    for (auto i = 0; i < n; i++)
      p[i] = i;
  }

  int findSet(int i)
  {
    return (p[i] == i) ? i : (p[i] = findSet(p[i]));
  }

  bool isSameSet(int i, int j)
  {
    return findSet(i) == findSet(j);
  }

  void unionSet(int i, int j)
  {
    if (!isSameSet(i, j))
    {
      numSets--;
      int x = findSet(i), y = findSet(j);
      // rank is used to keep the tree short
      if (rank[x] > rank[y])
      {
        p[y] = x;
        setSize[x] += setSize[y];
      }
      else
      {
        p[x] = y;
        setSize[y] += setSize[x];
        if (rank[x] == rank[y])
          rank[y]++;
      }
    }
  }

  int numDisjointSets()
  {
    return numSets;
  }

  int sizeOfSet(int i)
  {
    return setSize[findSet(i)];
  }
};
#endif
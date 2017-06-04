import re
cr = re.compile("closing connction ([^ \n]*)")
wr = re.compile("Writer task send to node_id ([^ \n]*)")
lr = re.compile("looping through")

p = {}
m = 0
mx =0
for i in open("/home/abhinav/log_2017-02-03_05-15.txt").xreadlines():
    a = cr.findall(i)
    if(a):
        if(a[0] in p):
                p[a[0]][1]+=1
        else:
                p[a[0]] = [0,1]
        continue
    b = wr.findall(i)
    if(b):
        if(b[0] in p):
                p[b[0]][0]+=1
        else:
                p[b[0]] = [1,0]
                
        continue
    
    if(lr.search(i)!=None):
        m+=1
        mx = max(m, mx)
        continue
    m=0


sorted_x = sorted(p.items(), key=lambda x: abs(x[1][0] - x[1][1]) , reverse=True)
for i in range(0, 100):
    print sorted_x[i]
    
print mx

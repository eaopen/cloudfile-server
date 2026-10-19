namespace Seafile {

public class WebAccess : Object {
       public string repo_id { set; get; }
       public string obj_id { set; get; }
       public string op { set; get; }
       public string username { set; get; }
       // Private RPC populates this only for an already-authorized file read.
       public string scope_path { set; get; }
}

}

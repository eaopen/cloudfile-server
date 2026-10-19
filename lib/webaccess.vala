namespace Seafile {

public class WebAccess : Object {
       public string repo_id { set; get; }
       public string obj_id { set; get; }
       public string op { set; get; }
       public string username { set; get; }
       // Trusted Hub's canonical path proof; reusable reads avoid ACL queries.
       public string scope_path { set; get; }
}

}
